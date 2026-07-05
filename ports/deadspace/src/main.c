#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include <GLES/gl.h>
#include <SDL2/SDL.h>

#include "imports.h"
#include "jni_shim.h"
#include "so_util.h"

#define SO_NAME "lib/libDeadSpace.so"
#define GAME_HEAP_MB 192
#define GAME_LOAD_SIZE 0x4e2000u
#define SFCALL __attribute__((pcs("aapcs")))

typedef int jint;
typedef unsigned char jboolean;

extern DynLibFunction revc_pthread_table[];
extern const int revc_pthread_count;
extern int _setjmp(void *);
extern void _longjmp(void *, int) __attribute__((noreturn));
extern unsigned deadspace_gl_take_draw_count(void);
extern void deadspace_gl_take_draw_stats(unsigned *total, unsigned *default_draws,
                                         unsigned *fbo_draws, unsigned *current_fbo);

#ifndef GL_FRAMEBUFFER_BINDING_OES
#define GL_FRAMEBUFFER_BINDING_OES 0x8CA6
#endif
#ifndef GL_TEXTURE_BINDING_2D
#define GL_TEXTURE_BINDING_2D 0x8069
#endif

volatile uintptr_t g_load_base = 0;

static void *g_env;
static void *g_vm;
static int g_running = 1;
static int g_w = 1280, g_h = 720;

static int control_log_enabled(void) {
  static int enabled = -1;
  if (enabled < 0) enabled = getenv("DS_CONTROLLOG") != NULL;
  return enabled;
}

static jint (*p_JNI_OnLoad)(void *vm, void *reserved);
static void (*p_EAIO_Startup)(void *env, void *clazz, void *asset_manager);
static void (*p_rwfs_Startup)(void *env, void *clazz, void *asset_manager);
static void (*p_Audio_Init)(void *env, void *clazz, void *audio_track, int frames, int chans, int rate);
static int (*p_runEntryPoint)(void *env, void *clazz);
static void (*p_NativeOnCreate)(void *env, void *thiz);
static void (*p_NativeOnResume)(void *env, void *thiz);
static void (*p_NativeOnPause)(void *env, void *thiz);
static void (*p_NativeOnWindowFocusChanged)(void *env, void *thiz, jboolean focused);
static void (*p_SurfaceCreated)(void *env, void *thiz);
static void (*p_SurfaceChanged)(void *env, void *thiz, int w, int h);
static void (*p_DrawFrame)(void *env, void *thiz);
static void (*p_KeyDown)(void *env, void *thiz, int module_id, int key_code, int alt);
static void (*p_KeyUp)(void *env, void *thiz, int module_id, int key_code, int alt);
static void (SFCALL *p_PointerEvent)(void *env, void *thiz, int raw_event, int module_id,
                                     int pointer_id, float x, float y);
static int (*p_ModPhysicalKeyboard)(void *env, void *thiz);
static int (*p_ModTouchScreen)(void *env, void *thiz);
static int (*p_ModTouchPad)(void *env, void *thiz);
static int (*p_RawDown)(void *env, void *thiz);
static int (*p_RawMove)(void *env, void *thiz);
static int (*p_RawUp)(void *env, void *thiz);
static int (*p_RawCancel)(void *env, void *thiz);

static int g_mod_key, g_mod_touch, g_mod_touchpad;
static int g_raw_down, g_raw_move, g_raw_up, g_raw_cancel;

static int env_flag(const char *name) {
  const char *v = getenv(name);
  return v && *v && strcmp(v, "0") != 0;
}

static int env_int(const char *name, int fallback) {
  const char *v = getenv(name);
  if (!v || !*v) return fallback;
  return atoi(v);
}

int deadspace_screen_width(void) { return g_w; }
int deadspace_screen_height(void) { return g_h; }

/* Android keycodes. */
enum {
  AK_BACK = 4, AK_DPAD_UP = 19, AK_DPAD_DOWN = 20, AK_DPAD_LEFT = 21,
  AK_DPAD_RIGHT = 22, AK_DPAD_CENTER = 23, AK_ENTER = 66,
  AK_BUTTON_A = 96, AK_BUTTON_B = 97, AK_BUTTON_X = 99, AK_BUTTON_Y = 100,
  AK_BUTTON_L1 = 102, AK_BUTTON_R1 = 103, AK_BUTTON_L2 = 104, AK_BUTTON_R2 = 105,
  AK_BUTTON_START = 108, AK_BUTTON_SELECT = 109, AK_BUTTON_THUMBL = 106,
  AK_BUTTON_THUMBR = 107, AK_BUTTON_MODE = 110
};

static SDL_AudioDeviceID g_audio_dev;
#define AUDIO_RING_SAMPLES (44100 * 2 * 4)
static int16_t *g_audio_ring;
static int g_audio_rpos, g_audio_wpos, g_audio_fill;
static int g_audio_paused = 1;

static int audio_log_enabled(void) {
  static int enabled = -1;
  if (enabled < 0) enabled = getenv("DS_AUDIOLOG") != NULL;
  return enabled;
}

static int audio_abs16(int v) {
  return v < 0 ? -v : v;
}

static int audio_max_fill_samples(int write_samples) {
  int max_fill = env_int("DS_AUDIO_MAX_FILL", 8192);
  if (max_fill < write_samples * 2) max_fill = write_samples * 2;
  if (max_fill > AUDIO_RING_SAMPLES) max_fill = AUDIO_RING_SAMPLES;
  return max_fill;
}

static void audio_cb(void *userdata, Uint8 *stream, int len) {
  (void)userdata;
  int samples = len / 2;
  int16_t *out = (int16_t *)stream;
  int peak = 0;
  int underrun = 0;
  for (int i = 0; i < samples; i++) {
    if (g_audio_fill > 0) {
      out[i] = g_audio_ring[g_audio_rpos];
      int a = audio_abs16(out[i]);
      if (a > peak) peak = a;
      g_audio_rpos = (g_audio_rpos + 1) % AUDIO_RING_SAMPLES;
      g_audio_fill--;
    } else {
      out[i] = 0;
      underrun++;
    }
  }
  if (audio_log_enabled()) {
    static unsigned calls;
    static unsigned underruns_total;
    calls++;
    if (underrun) underruns_total++;
    if (calls <= 16 || (calls % 256) == 0) {
      fprintf(stderr, "[audio] cb samples=%d peak=%d fill=%d underrun=%d/%u paused=%d call=%u\n",
              samples, peak, g_audio_fill, underrun, underruns_total, g_audio_paused, calls);
    }
  }
}

static void audio_write(const int16_t *samples, int sample_count) {
  if (!g_audio_dev || !g_audio_ring || !samples || sample_count <= 0) return;
  int peak = 0;
  int max_fill = audio_max_fill_samples(sample_count);
  while (g_running && !g_audio_paused) {
    SDL_LockAudioDevice(g_audio_dev);
    int fill = g_audio_fill;
    SDL_UnlockAudioDevice(g_audio_dev);
    if (fill + sample_count <= max_fill) break;
    SDL_Delay(1);
  }
  SDL_LockAudioDevice(g_audio_dev);
  for (int i = 0; i < sample_count; i++) {
    int a = audio_abs16(samples[i]);
    if (a > peak) peak = a;
    if (g_audio_fill >= AUDIO_RING_SAMPLES) {
      g_audio_rpos = (g_audio_rpos + 1) % AUDIO_RING_SAMPLES;
      g_audio_fill--;
    }
    g_audio_ring[g_audio_wpos] = samples[i];
    g_audio_wpos = (g_audio_wpos + 1) % AUDIO_RING_SAMPLES;
    g_audio_fill++;
  }
  int fill = g_audio_fill;
  SDL_UnlockAudioDevice(g_audio_dev);
  if (audio_log_enabled()) {
    static unsigned writes;
    writes++;
    if (writes <= 96 || (writes % 256) == 0)
      fprintf(stderr, "[audio] queued samples=%d peak=%d fill=%d paused=%d call=%u\n",
              sample_count, peak, fill, g_audio_paused, writes);
  }
}

static void audio_state(int playing) {
  if (!g_audio_dev) return;
  if (playing == 2) {
    SDL_LockAudioDevice(g_audio_dev);
    g_audio_rpos = 0;
    g_audio_wpos = 0;
    g_audio_fill = 0;
    SDL_UnlockAudioDevice(g_audio_dev);
    if (audio_log_enabled()) fprintf(stderr, "[audio] flush ring paused=%d\n", g_audio_paused);
    return;
  }
  g_audio_paused = playing ? 0 : 1;
  SDL_PauseAudioDevice(g_audio_dev, g_audio_paused);
  if (audio_log_enabled()) fprintf(stderr, "[audio] state %s\n", playing ? "play" : "pause");
}

static void resolve_addr(uintptr_t a, char *out, int outsz) {
  int fd = open("/proc/self/maps", O_RDONLY);
  out[0] = 0;
  if (fd < 0) return;
  char buf[8192], line[512];
  int n, li = 0;
  while ((n = read(fd, buf, sizeof(buf))) > 0) {
    for (int i = 0; i < n; i++) {
      char c = buf[i];
      if (c == '\n' || li >= (int)sizeof(line) - 1) {
        line[li] = 0;
        unsigned long s, e;
        char perm[16], path[256] = "";
        if (sscanf(line, "%lx-%lx %15s %*x %*s %*d %255s", &s, &e, perm, path) >= 3) {
          if (a >= s && a < e) {
            const char *base = strrchr(path, '/');
            base = base ? base + 1 : (path[0] ? path : "?");
            snprintf(out, outsz, "%s+0x%lx", base, (unsigned long)(a - s));
            close(fd);
            return;
          }
        }
        li = 0;
      } else {
        line[li++] = c;
      }
    }
  }
  close(fd);
}

static void crash_handler(int sig, siginfo_t *info, void *uc) {
  ucontext_t *u = (ucontext_t *)uc;
  mcontext_t *m = &u->uc_mcontext;
  char pcbuf[256], lrbuf[256];
  resolve_addr(m->arm_pc, pcbuf, sizeof(pcbuf));
  resolve_addr(m->arm_lr, lrbuf, sizeof(lrbuf));
  if (sig == SIGSEGV && info && info->si_code <= 0 && !getenv("DS_FATAL_USER_SIGSEGV")) {
    static volatile int suppressed;
    int n = __sync_add_and_fetch(&suppressed, 1);
    if (n <= 16) {
      fprintf(stderr,
              "\n=== DEADSPACE suppressed generated SIGSEGV code=%d addr=%p tid=%d "
              "PC=0x%lx %s LR=0x%lx %s ===\n",
              info->si_code, info->si_addr, (int)syscall(__NR_gettid),
              (unsigned long)m->arm_pc, pcbuf, (unsigned long)m->arm_lr, lrbuf);
    }
    if (n < 256) return;
  }
  fprintf(stderr, "\n=== DEADSPACE CRASH sig=%d code=%d addr=%p tid=%d ===\n",
          sig, info ? info->si_code : 0, info ? info->si_addr : NULL,
          (int)syscall(__NR_gettid));
  fprintf(stderr, "PC=0x%lx %s", (unsigned long)m->arm_pc, pcbuf);
  if (g_load_base && m->arm_pc >= g_load_base && m->arm_pc < g_load_base + GAME_LOAD_SIZE)
    fprintf(stderr, " {%s+0x%lx}", SO_NAME, (unsigned long)(m->arm_pc - g_load_base));
  fprintf(stderr, "\nLR=0x%lx %s", (unsigned long)m->arm_lr, lrbuf);
  if (g_load_base && m->arm_lr >= g_load_base && m->arm_lr < g_load_base + GAME_LOAD_SIZE)
    fprintf(stderr, " {%s+0x%lx}", SO_NAME, (unsigned long)(m->arm_lr - g_load_base));
  fprintf(stderr, "\nr0=%08lx r1=%08lx r2=%08lx r3=%08lx r4=%08lx r5=%08lx\n",
          (unsigned long)m->arm_r0, (unsigned long)m->arm_r1,
          (unsigned long)m->arm_r2, (unsigned long)m->arm_r3,
          (unsigned long)m->arm_r4, (unsigned long)m->arm_r5);
  fprintf(stderr, "sp=%08lx fp=%08lx ip=%08lx\n",
          (unsigned long)m->arm_sp, (unsigned long)m->arm_fp, (unsigned long)m->arm_ip);
  fprintf(stderr, "stack refs:\n");
  int printed = 0;
  for (uintptr_t a = m->arm_sp; a < m->arm_sp + 0x1200 && printed < 32; a += 4) {
    uintptr_t v = *(uintptr_t *)a;
    char rb[256];
    rb[0] = 0;
    if (g_load_base && v >= g_load_base && v < g_load_base + GAME_LOAD_SIZE) {
      fprintf(stderr, "  [sp+0x%lx] %s+0x%lx\n", (unsigned long)(a - m->arm_sp),
              SO_NAME, (unsigned long)(v - g_load_base));
      printed++;
    } else if (v > 0x10000) {
      resolve_addr(v, rb, sizeof(rb));
      if (rb[0] && strstr(rb, ".so")) {
        fprintf(stderr, "  [sp+0x%lx] 0x%lx %s\n", (unsigned long)(a - m->arm_sp),
                (unsigned long)v, rb);
        printed++;
      }
    }
  }
  fflush(stderr);
  _exit(128 + sig);
}

static void install_crash_handler(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = crash_handler;
  sa.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);
  sigaction(SIGILL, &sa, NULL);
  sigaction(SIGABRT, &sa, NULL);
}

static void preload_device_libs(void) {
  const char *libs[] = {
      "libSDL2-2.0.so.0", "libGLESv1_CM.so.1", "libGLESv1_CM.so",
      "libEGL.so.1", "libEGL.so", "libstdc++.so.6", "libgcc_s.so.1",
      "libm.so.6", "libdl.so.2", NULL};
  for (int i = 0; libs[i]; i++) {
    void *h = dlopen(libs[i], RTLD_NOW | RTLD_GLOBAL);
    fprintf(stderr, "preload: %s %s\n", libs[i], h ? "OK" : dlerror());
  }
}

static DynLibFunction *build_import_table(int *out_n) {
  int n = deadspace_overrides_count + revc_pthread_count;
  DynLibFunction *t = calloc((size_t)n, sizeof(*t));
  int k = 0;
  for (int i = 0; i < deadspace_overrides_count; i++) t[k++] = deadspace_overrides[i];
  for (int i = 0; i < revc_pthread_count; i++) t[k++] = revc_pthread_table[i];
  *out_n = k;
  return t;
}

static void patch_got_slot(const char *symbol, uintptr_t value) {
  uintptr_t slot = so_find_rel_addr_safe(symbol);
  if (slot) {
    *(uintptr_t *)slot = value;
    fprintf(stderr, "patched %s GOT -> %p\n", symbol, (void *)value);
  }
}

static uint32_t arm_branch(uintptr_t from, uintptr_t to, uint32_t opcode) {
  intptr_t off = (intptr_t)to - (intptr_t)from - 8;
  if ((off & 3) || off < -33554432 || off > 33554428) {
    fprintf(stderr, "ARM branch out of range: %p -> %p\n", (void *)from, (void *)to);
  }
  return opcode | (((uint32_t)(off >> 2)) & 0x00ffffffu);
}

static void write_abs_jump_stub(uintptr_t stub_addr, uintptr_t target) {
  uint32_t *s = (uint32_t *)stub_addr;
  s[0] = 0xe59fc000u; /* ldr ip, [pc] */
  s[1] = 0xe12fff1cu; /* bx ip */
  s[2] = (uint32_t)target;
  __builtin___clear_cache((char *)stub_addr, (char *)stub_addr + 12);
}

static void patch_arm_entry(uintptr_t entry, uintptr_t stub_addr) {
  *(uint32_t *)entry = arm_branch(entry, stub_addr, 0xea000000u);
  __builtin___clear_cache((char *)entry, (char *)entry + 4);
}

static void core_allocator_dtor(void *self) {
  (void)self;
}

static unsigned next_pow2_u(unsigned v) {
  if (v < sizeof(void *)) v = sizeof(void *);
  v--;
  v |= v >> 1;
  v |= v >> 2;
  v |= v >> 4;
  v |= v >> 8;
  v |= v >> 16;
  return v + 1;
}

static void *core_allocator_alloc_impl(size_t size, unsigned align, unsigned offset) {
  if (size == 0) size = 1;
  align = next_pow2_u(align);
  size_t total = size + align + sizeof(void *);
  void *raw = malloc(total);
  if (!raw) return NULL;
  uintptr_t base = (uintptr_t)raw + sizeof(void *);
  uintptr_t p = (base + offset + align - 1u) & ~(uintptr_t)(align - 1u);
  p -= offset;
  ((void **)p)[-1] = raw;
  return (void *)p;
}

static void *core_allocator_alloc(void *self, uint32_t size, const char *name, uint32_t flags) {
  (void)self;
  (void)name;
  (void)flags;
  return core_allocator_alloc_impl(size, 8, 0);
}

static void *core_allocator_alloc_aligned(void *self, uint32_t size, const char *name,
                                          uint32_t flags, uint32_t align,
                                          uint32_t offset) {
  (void)self;
  (void)name;
  (void)flags;
  return core_allocator_alloc_impl(size, align ? align : 8, offset);
}

static void core_allocator_free(void *self, void *ptr, uint32_t size) {
  (void)self;
  (void)size;
  if (ptr) free(((void **)ptr)[-1]);
}

static void *core_allocator_alloc_debug(void *self, uint32_t size, void *debug_params,
                                        uint32_t flags, uint32_t align, uint32_t offset) {
  (void)debug_params;
  return core_allocator_alloc_aligned(self, size, NULL, flags, align, offset);
}

static uintptr_t g_core_allocator_vtable[] = {
    (uintptr_t)core_allocator_dtor,
    (uintptr_t)core_allocator_dtor,
    (uintptr_t)core_allocator_alloc,
    (uintptr_t)core_allocator_alloc_aligned,
    (uintptr_t)core_allocator_free,
    (uintptr_t)core_allocator_alloc_debug,
    (uintptr_t)core_allocator_alloc_debug,
};
static uintptr_t g_core_allocator_obj[] = {(uintptr_t)g_core_allocator_vtable};

static void *fontfusion_malloc_bridge(uint32_t size) {
  if (size == 0) size = 1;
  return malloc(size);
}

static void fontfusion_free_bridge(void *ptr) {
  free(ptr);
}

static void *fontfusion_realloc_bridge(void *ptr, int size) {
  if (size <= 0) size = 1;
  return realloc(ptr, (size_t)size);
}

static void *tsi_alloc_bridge(void *memhandler, uint32_t size) {
  (void)memhandler;
  uint8_t *raw = malloc((size_t)size + 18u);
  if (!raw) return NULL;
  *(uint32_t *)(raw + 0) = 0xaa53c5aau;
  *(uint32_t *)(raw + 4) = size;
  *(uint32_t *)(raw + 8) = 0;
  *(uint32_t *)(raw + 12) = 0;
  raw[16 + size] = 0x5a;
  raw[17 + size] = 0xf0;
  return raw + 16;
}

static void tsi_dealloc_bridge(void *memhandler, void *ptr) {
  (void)memhandler;
  if (!ptr) return;
  uint8_t *raw = (uint8_t *)ptr - 16;
  free(raw);
}

static void *tsi_realloc_bridge(void *memhandler, void *ptr, uint32_t size) {
  (void)memhandler;
  if (!ptr) return tsi_alloc_bridge(memhandler, size);
  uint8_t *old_raw = (uint8_t *)ptr - 16;
  uint8_t *raw = realloc(old_raw, (size_t)size + 18u);
  if (!raw) return NULL;
  *(uint32_t *)(raw + 0) = 0xaa53c5aau;
  *(uint32_t *)(raw + 4) = size;
  raw[16 + size] = 0x5a;
  raw[17 + size] = 0xf0;
  return raw + 16;
}

static void *tsi_fast_alloc_bridge(void *memhandler, uint32_t size, uint32_t slot) {
  (void)slot;
  return tsi_alloc_bridge(memhandler, size);
}

static void install_fontfusion_allocator_patch(void) {
  if (!g_load_base) return;

  uintptr_t tsi_dealloc_entry = g_load_base + 0x3f12e8u;
  uintptr_t tsi_alloc_entry = g_load_base + 0x3f1530u;
  uintptr_t tsi_fast_alloc_entry = g_load_base + 0x3f164cu;
  uintptr_t tsi_realloc_entry = g_load_base + 0x3f16c0u;
  uintptr_t ff_malloc_entry = g_load_base + 0x3f20ccu;
  uintptr_t ff_free_entry = g_load_base + 0x3f20ecu;
  uintptr_t ff_realloc_entry = g_load_base + 0x3f2110u;
  uintptr_t tsi_dealloc_stub = g_load_base + 0x4f0430u;
  uintptr_t tsi_alloc_stub = g_load_base + 0x4f0440u;
  uintptr_t tsi_realloc_stub = g_load_base + 0x4f0450u;
  uintptr_t tsi_fast_alloc_stub = g_load_base + 0x4f0460u;
  uintptr_t malloc_stub = g_load_base + 0x4f0400u;
  uintptr_t free_stub = g_load_base + 0x4f0410u;
  uintptr_t realloc_stub = g_load_base + 0x4f0420u;

  write_abs_jump_stub(malloc_stub, (uintptr_t)fontfusion_malloc_bridge);
  write_abs_jump_stub(free_stub, (uintptr_t)fontfusion_free_bridge);
  write_abs_jump_stub(realloc_stub, (uintptr_t)fontfusion_realloc_bridge);
  write_abs_jump_stub(tsi_dealloc_stub, (uintptr_t)tsi_dealloc_bridge);
  write_abs_jump_stub(tsi_alloc_stub, (uintptr_t)tsi_alloc_bridge);
  write_abs_jump_stub(tsi_realloc_stub, (uintptr_t)tsi_realloc_bridge);
  write_abs_jump_stub(tsi_fast_alloc_stub, (uintptr_t)tsi_fast_alloc_bridge);
  patch_arm_entry(tsi_dealloc_entry, tsi_dealloc_stub);
  patch_arm_entry(tsi_alloc_entry, tsi_alloc_stub);
  patch_arm_entry(tsi_realloc_entry, tsi_realloc_stub);
  patch_arm_entry(tsi_fast_alloc_entry, tsi_fast_alloc_stub);
  patch_arm_entry(ff_malloc_entry, malloc_stub);
  patch_arm_entry(ff_free_entry, free_stub);
  patch_arm_entry(ff_realloc_entry, realloc_stub);

  fprintf(stderr, "installed FontFusion allocator patch tsi=%p/%p/%p/%p ff=%p/%p/%p\n",
          (void *)tsi_alloc_entry, (void *)tsi_dealloc_entry, (void *)tsi_fast_alloc_entry,
          (void *)tsi_realloc_entry,
          (void *)ff_malloc_entry, (void *)ff_free_entry, (void *)ff_realloc_entry);
}

typedef struct {
  uint32_t begin;
  uint32_t end;
  uint32_t capacity_end;
  uint32_t allocator0;
  uint32_t allocator1;
} GameWString;

static uintptr_t g_posix_fs_obj[1];
static uintptr_t g_posix_ref[5];

static void game_wstring_to_utf8(const void *s, char *out, size_t outsz) {
  if (!out || outsz == 0) return;
  out[0] = 0;
  if (!s) return;
  const uint32_t *w = (const uint32_t *)s;
  const uint16_t *p = (const uint16_t *)(uintptr_t)w[0];
  const uint16_t *e = (const uint16_t *)(uintptr_t)w[1];
  if (!p || !e || e < p) return;
  size_t n = 0;
  while (p < e && n + 1 < outsz) {
    uint16_t c = *p++;
    out[n++] = (c >= 32 && c < 127) ? (char)c : '?';
  }
  out[n] = 0;
}

static uint16_t *utf8_to_game_wchar_dup(const char *s, size_t *out_len) {
  size_t n = strlen(s);
  uint16_t *w = calloc(n + 1, sizeof(*w));
  if (!w) return NULL;
  for (size_t i = 0; i < n; i++) w[i] = (uint8_t)s[i];
  if (out_len) *out_len = n;
  return w;
}

static int game_wstring_init(GameWString *out, const char *s) {
  if (!g_load_base || !out || !s) return 0;

  void (*alloc_ctor)(void *, const char *) = (void *)(g_load_base + 0x348210u);
  void (*range_init)(void *, const uint16_t *, const uint16_t *) =
      (void *)(g_load_base + 0x3dbb0u);

  uint32_t alloc[2] = {0, 0};
  size_t len = 0;
  uint16_t *w = utf8_to_game_wchar_dup(s, &len);
  if (!w) return 0;

  memset(out, 0, sizeof(*out));
  alloc_ctor(alloc, "NextOSDeadSpaceVFS");
  out->allocator0 = alloc[0];
  out->allocator1 = alloc[1];
  range_init(out, w, w + len);
  free(w);
  return 1;
}

static void ensure_posix_fs_bridge(void) {
  if (!g_load_base) return;
  if (g_posix_fs_obj[0]) return;
  g_posix_fs_obj[0] = g_load_base + 0x48cdc8u;
  g_posix_ref[0] = g_load_base + 0x48ca88u;
  g_posix_ref[1] = 1;
  g_posix_ref[2] = 1;
  g_posix_ref[3] = 0;
  g_posix_ref[4] = (uintptr_t)g_posix_fs_obj;
}

static int published_suffix(const char *path, const char **suffix) {
  if (!path || !suffix) return 0;
  if (strncmp(path, "/published/", 11) == 0) {
    *suffix = path + 11;
    return **suffix != 0;
  }
  if (strncmp(path, "published/", 10) == 0) {
    *suffix = path + 10;
    return **suffix != 0;
  }
  return 0;
}

static int make_published_physical_path(const char *path8, char *phys, size_t phys_sz) {
  const char *suffix = NULL;
  if (!phys || phys_sz == 0 || !published_suffix(path8, &suffix)) return 0;

  const char *assets = getenv("DEADSPACE_ASSETS");
  if (!assets || !*assets) assets = "assets";

  snprintf(phys, phys_sz, "%s/published/%s", assets, suffix);
  return 1;
}

static int vfs_open_published_physical(void *out, const char *path8, int log) {
  if (!out) return 0;

  char phys[1024];
  if (!make_published_physical_path(path8, phys, sizeof(phys))) return 0;

  GameWString phys_s;
  if (!game_wstring_init(&phys_s, phys)) {
    if (log) fprintf(stderr, "[vfs] published fallback string failed: %s\n", phys);
    return 0;
  }

  ensure_posix_fs_bridge();
  typedef void *(*PosixOpenFn)(void *, void *, void *);
  PosixOpenFn posix_open = (PosixOpenFn)(g_load_base + 0x37c0f8u);
  posix_open(out, g_posix_fs_obj, &phys_s);

  uintptr_t stream = *(uintptr_t *)out;
  uintptr_t ctrl = *((uintptr_t *)out + 1);
  if (log) {
    fprintf(stderr, "[vfs] published fallback %s -> stream=%p ctrl=%p\n",
            phys, (void *)stream, (void *)ctrl);
  }
  return stream != 0;
}

static int vfs_get_published_file_info(void *info, const char *path8, int log) {
  if (!info) return 0;

  char phys[1024];
  if (!make_published_physical_path(path8, phys, sizeof(phys))) return 0;

  GameWString phys_s;
  if (!game_wstring_init(&phys_s, phys)) {
    if (log) fprintf(stderr, "[vfs] getFileInfo string failed: %s\n", phys);
    return 0;
  }

  ensure_posix_fs_bridge();
  typedef int (*PosixInfoFn)(void *, void *, void *);
  PosixInfoFn posix_info = (PosixInfoFn)(g_load_base + 0x37bae4u);
  int ok = posix_info(g_posix_fs_obj, &phys_s, info);
  if (log) {
    fprintf(stderr, "[vfs] getFileInfo fallback %s -> %d\n", phys, ok);
  }
  return ok;
}

static void *vfs_open_input_stream_hook(void *out, void *self, void *path) {
  typedef void *(*OrigFn)(void *, void *, void *);
  OrigFn orig = (OrigFn)(g_load_base + 0x35f6f4u);
  char path8[512];
  int log = getenv("DS_VFSLOG") != NULL;
  game_wstring_to_utf8(path, path8, sizeof(path8));
  if (log) {
    fprintf(stderr, "[vfs] openInputStream %s\n", path8);
  }
  void *ret = orig(out, self, path);
  if (out && *(uintptr_t *)out == 0 && path8[0]) {
    vfs_open_published_physical(out, path8, log);
  }
  if (log) {
    uintptr_t stream = out ? *(uintptr_t *)out : 0;
    uintptr_t ctrl = out ? *((uintptr_t *)out + 1) : 0;
    fprintf(stderr, "[vfs] -> stream=%p ctrl=%p\n", (void *)stream, (void *)ctrl);
  }
  return ret;
}

static int vfs_get_file_info_hook(void *self, void *path, void *info) {
  typedef int (*OrigFn)(void *, void *, void *);
  OrigFn orig = (OrigFn)(g_load_base + 0x35f014u);
  char path8[512];
  int log = getenv("DS_VFSLOG") != NULL;
  game_wstring_to_utf8(path, path8, sizeof(path8));
  int ok = orig(self, path, info);
  if (!ok && path8[0]) ok = vfs_get_published_file_info(info, path8, log);
  if (log) {
    fprintf(stderr, "[vfs] getFileInfo %s -> %d\n", path8, ok);
  }
  return ok;
}

static void install_vfs_open_hook(void) {
  if (!g_load_base) return;
  uintptr_t open_slot = g_load_base + 0x48bfd0u; /* im::VFS vtable openInputStream */
  uintptr_t info_slot = g_load_base + 0x48bfe0u; /* im::VFS vtable getFileInfo */
  *(uintptr_t *)open_slot = (uintptr_t)vfs_open_input_stream_hook;
  *(uintptr_t *)info_slot = (uintptr_t)vfs_get_file_info_hook;
  fprintf(stderr, "installed VFS hooks open=%p info=%p\n",
          (void *)open_slot, (void *)info_slot);
}

static void install_filestream_published_patch(void) {
  if (!g_load_base) return;
  uintptr_t patch = g_load_base + 0x272e70u;
  uintptr_t normal_open = g_load_base + 0x272e84u;
  *(uint32_t *)patch = arm_branch(patch, normal_open, 0xea000000u);
  __builtin___clear_cache((char *)patch, (char *)patch + 4);
  fprintf(stderr, "installed FileStream /published POSIX patch=%p target=%p\n",
          (void *)patch, (void *)normal_open);
}

static void install_core_allocator_patch(void) {
  if (!g_load_base) return;

  uintptr_t patch = g_load_base + 0x36fd50u;     /* EA::core::GetAllocator() */
  uintptr_t stub_addr = g_load_base + 0x4f0300u;
  uintptr_t get_default = g_load_base + 0x27e560u; /* EA::Allocator::ICoreAllocator::GetDefaultAllocator() */

  write_abs_jump_stub(stub_addr, get_default);
  patch_arm_entry(patch, stub_addr);
  fprintf(stderr, "installed EA::core allocator patch=%p stub=%p default=%p\n",
          (void *)patch, (void *)stub_addr, (void *)get_default);
}

static void install_texturepack_empty_guard(void) {
  if (!g_load_base) return;

  uintptr_t patch = g_load_base + 0x35b564u;
  uintptr_t stub_addr = g_load_base + 0x4f0000u;
  uintptr_t cleanup = g_load_base + 0x35c5f8u;
  uintptr_t resume = g_load_base + 0x35b580u;
  uintptr_t get_user_data = g_load_base + 0x368338u;
  uint32_t *s = (uint32_t *)stub_addr;
  int i = 0;

  s[i++] = 0xe59a3000u; /* ldr r3, [sl] */
  s[i++] = 0xe3530000u; /* cmp r3, #0 */
  s[i] = arm_branch((uintptr_t)&s[i], cleanup, 0x0a000000u); i++; /* beq cleanup */
  s[i++] = 0xe5931000u; /* ldr r1, [r3] */
  s[i++] = 0xe3510000u; /* cmp r1, #0 */
  s[i] = arm_branch((uintptr_t)&s[i], cleanup, 0x0a000000u); i++; /* beq cleanup */
  s[i++] = 0xe28d2ffbu; /* add r2, sp, #1004 */
  s[i++] = 0xe58d2058u; /* str r2, [sp, #88] */
  s[i++] = 0xe3a02f7du; /* mov r2, #500 */
  s[i++] = 0xe59d0058u; /* ldr r0, [sp, #88] */
  s[i] = arm_branch((uintptr_t)&s[i], get_user_data, 0xeb000000u); i++; /* bl getUserData */
  s[i] = arm_branch((uintptr_t)&s[i], resume, 0xea000000u); i++;        /* b resume */

  __builtin___clear_cache((char *)stub_addr, (char *)stub_addr + i * 4);
  *(uint32_t *)patch = arm_branch(patch, stub_addr, 0xea000000u);
  __builtin___clear_cache((char *)patch, (char *)patch + 4);
  fprintf(stderr, "installed TexturePack empty-vector guard patch=%p stub=%p\n",
          (void *)patch, (void *)stub_addr);
}

static void install_cinematic_model_null_guard(void) {
  if (!g_load_base) return;

  uintptr_t patch = g_load_base + 0x10fe38u;
  uintptr_t stub_addr = g_load_base + 0x4f0500u;
  uintptr_t resume = g_load_base + 0x10fe50u;
  uint32_t *s = (uint32_t *)stub_addr;
  int i = 0;

  s[i++] = 0xe3530000u; /* cmp r3, #0 */
  s[i] = arm_branch((uintptr_t)&s[i], resume, 0x0a000000u); i++; /* beq resume */
  s[i++] = 0xe583207cu; /* str r2, [r3, #124] */
  s[i++] = 0xe594304cu; /* ldr r3, [r4, #76] */
  s[i++] = 0xe284200cu; /* add r2, r4, #12 */
  s[i++] = 0xe3530000u; /* cmp r3, #0 */
  s[i] = arm_branch((uintptr_t)&s[i], resume, 0x0a000000u); i++; /* beq resume */
  s[i++] = 0xe5933018u; /* ldr r3, [r3, #24] */
  s[i++] = 0xe3530000u; /* cmp r3, #0 */
  s[i++] = 0x15832080u; /* strne r2, [r3, #128] */
  s[i] = arm_branch((uintptr_t)&s[i], resume, 0xea000000u); i++; /* b resume */

  __builtin___clear_cache((char *)stub_addr, (char *)stub_addr + i * 4);
  patch_arm_entry(patch, stub_addr);
  fprintf(stderr, "installed cinematic model null guard patch=%p stub=%p\n",
          (void *)patch, (void *)stub_addr);
}

static void install_hemisphere_map_null_guard(void) {
  if (!g_load_base) return;

  uintptr_t patch = g_load_base + 0x57460u;
  uintptr_t stub_addr = g_load_base + 0x4f0580u;
  uintptr_t duplicate = g_load_base + 0x368efcu;
  uintptr_t resume = g_load_base + 0x57464u;
  uintptr_t epilogue = g_load_base + 0x575ccu;
  uint32_t *s = (uint32_t *)stub_addr;
  int i = 0;

  s[i++] = 0xe3500000u; /* cmp r0, #0 */
  s[i] = arm_branch((uintptr_t)&s[i], epilogue, 0x0a000000u); i++; /* beq epilogue */
  s[i] = arm_branch((uintptr_t)&s[i], duplicate, 0xeb000000u); i++; /* bl duplicate */
  s[i] = arm_branch((uintptr_t)&s[i], resume, 0xea000000u); i++;    /* b resume */

  __builtin___clear_cache((char *)stub_addr, (char *)stub_addr + i * 4);
  patch_arm_entry(patch, stub_addr);
  fprintf(stderr, "installed HemisphereMap null guard patch=%p stub=%p\n",
          (void *)patch, (void *)stub_addr);
}

static void install_animplayer_setnode_null_guard(void) {
  if (!g_load_base) return;

  uintptr_t patch = g_load_base + 0x53284u;
  uintptr_t stub_addr = g_load_base + 0x4f05c0u;
  uintptr_t resume = g_load_base + 0x53288u;
  uint32_t *s = (uint32_t *)stub_addr;
  int i = 0;

  s[i++] = 0xe3500000u; /* cmp r0, #0 */
  s[i++] = 0x012fff1eu; /* bxeq lr */
  s[i++] = 0xe92d4ff0u; /* push {r4, r5, r6, r7, r8, r9, sl, fp, lr} */
  s[i] = arm_branch((uintptr_t)&s[i], resume, 0xea000000u); i++; /* b resume */

  __builtin___clear_cache((char *)stub_addr, (char *)stub_addr + i * 4);
  patch_arm_entry(patch, stub_addr);
  fprintf(stderr, "installed AnimPlayer3D::setNode null guard patch=%p stub=%p\n",
          (void *)patch, (void *)stub_addr);
}

static void install_playable_rig_model_null_guard(void) {
  if (!g_load_base) return;

  uintptr_t patch = g_load_base + 0x191df8u;
  uintptr_t stub_addr = g_load_base + 0x4f0600u;
  uintptr_t resume = g_load_base + 0x191e14u;
  uint32_t *s = (uint32_t *)stub_addr;
  int i = 0;

  s[i++] = 0xe3530000u; /* cmp r3, #0 */
  s[i] = arm_branch((uintptr_t)&s[i], stub_addr + 10 * 4, 0x0a000000u); i++; /* beq setup */
  s[i++] = 0xe583007cu; /* str r0, [r3, #124] */
  s[i++] = 0xe594304cu; /* ldr r3, [r4, #76] */
  s[i++] = 0xe59d103cu; /* ldr r1, [sp, #60] */
  s[i++] = 0xe3530000u; /* cmp r3, #0 */
  s[i] = arm_branch((uintptr_t)&s[i], stub_addr + 10 * 4, 0x0a000000u); i++; /* beq setup */
  s[i++] = 0xe5933018u; /* ldr r3, [r3, #24] */
  s[i++] = 0xe3530000u; /* cmp r3, #0 */
  s[i++] = 0x15831080u; /* strne r1, [r3, #128] */
  s[i++] = 0xe28d0ff7u; /* add r0, sp, #988 */
  s[i] = arm_branch((uintptr_t)&s[i], resume, 0xea000000u); i++; /* b resume */

  __builtin___clear_cache((char *)stub_addr, (char *)stub_addr + i * 4);
  patch_arm_entry(patch, stub_addr);
  fprintf(stderr, "installed playable rig model null guard patch=%p stub=%p\n",
          (void *)patch, (void *)stub_addr);
}

static void install_animplayer_offset_null_guard(void) {
  if (!g_load_base) return;

  uintptr_t patch = g_load_base + 0x51910u;
  uintptr_t stub_addr = g_load_base + 0x4f0660u;
  uint32_t *s = (uint32_t *)stub_addr;
  int i = 0;

  s[i++] = 0xe3500000u; /* cmp r0, #0 */
  s[i++] = 0x012fff1eu; /* bxeq lr */
  s[i++] = 0xe5802098u; /* str r2, [r0, #152] */
  s[i++] = 0xe5801094u; /* str r1, [r0, #148] */
  s[i++] = 0xe12fff1eu; /* bx lr */

  __builtin___clear_cache((char *)stub_addr, (char *)stub_addr + i * 4);
  patch_arm_entry(patch, stub_addr);
  fprintf(stderr, "installed AnimPlayer3D::setOffsetNode null guard patch=%p stub=%p\n",
          (void *)patch, (void *)stub_addr);
}

static void install_animplayer_setanim_null_guard(void) {
  if (!g_load_base) return;

  uintptr_t patch = g_load_base + 0x54594u;
  uintptr_t stub_addr = g_load_base + 0x4f06c0u;
  uintptr_t resume = g_load_base + 0x545a0u;
  uint32_t *s = (uint32_t *)stub_addr;
  int i = 0;

  s[i++] = 0xe3500000u; /* cmp r0, #0 */
  s[i++] = 0x012fff1eu; /* bxeq lr */
  s[i++] = 0xe92d4ff8u; /* push {r3, r4, r5, r6, r7, r8, r9, sl, fp, lr} */
  s[i++] = 0xed2d8b02u; /* vpush {d8} */
  s[i++] = 0xe1a04000u; /* mov r4, r0 */
  s[i] = arm_branch((uintptr_t)&s[i], resume, 0xea000000u); i++; /* b resume */

  __builtin___clear_cache((char *)stub_addr, (char *)stub_addr + i * 4);
  patch_arm_entry(patch, stub_addr);
  fprintf(stderr, "installed AnimPlayer3D::setAnim null guard patch=%p stub=%p\n",
          (void *)patch, (void *)stub_addr);
}

static void install_plasmacutter_model_null_guard(void) {
  if (!g_load_base) return;

  uintptr_t patch = g_load_base + 0x1f6314u;
  uintptr_t stub_addr = g_load_base + 0x4f06a0u;
  uintptr_t resume = g_load_base + 0x1f6318u;
  uint32_t *s = (uint32_t *)stub_addr;
  int i = 0;

  s[i++] = 0xe3530000u; /* cmp r3, #0 */
  s[i++] = 0x1583207cu; /* strne r2, [r3, #124] */
  s[i] = arm_branch((uintptr_t)&s[i], resume, 0xea000000u); i++; /* b resume */

  __builtin___clear_cache((char *)stub_addr, (char *)stub_addr + i * 4);
  patch_arm_entry(patch, stub_addr);
  fprintf(stderr, "installed PlasmaCutter model null guard patch=%p stub=%p\n",
          (void *)patch, (void *)stub_addr);
}

static void install_ripper_model_null_guard(void) {
  if (!g_load_base) return;

  uintptr_t patch = g_load_base + 0x1fa3b4u;
  uintptr_t stub_addr = g_load_base + 0x4f06e0u;
  uintptr_t resume = g_load_base + 0x1fa3b8u;
  uint32_t *s = (uint32_t *)stub_addr;
  int i = 0;

  s[i++] = 0xe3530000u; /* cmp r3, #0 */
  s[i++] = 0x1583207cu; /* strne r2, [r3, #124] */
  s[i] = arm_branch((uintptr_t)&s[i], resume, 0xea000000u); i++; /* b resume */

  __builtin___clear_cache((char *)stub_addr, (char *)stub_addr + i * 4);
  patch_arm_entry(patch, stub_addr);
  fprintf(stderr, "installed Ripper model null guard patch=%p stub=%p\n",
          (void *)patch, (void *)stub_addr);
}

static void install_accelerometer_null_guard(void) {
  if (!g_load_base) return;

  uintptr_t patch = g_load_base + 0x370c20u;
  uintptr_t stub_addr = g_load_base + 0x4f0100u;
  uintptr_t ret_null = g_load_base + 0x370bf8u;
  uintptr_t resume = g_load_base + 0x370bd8u;
  uint32_t *s = (uint32_t *)stub_addr;
  int i = 0;

  s[i++] = 0xe1a05000u; /* mov r5, r0 */
  s[i++] = 0xe58400a8u; /* str r0, [r4, #168] */
  s[i++] = 0xe3500000u; /* cmp r0, #0 */
  s[i] = arm_branch((uintptr_t)&s[i], ret_null, 0x0a000000u); i++; /* beq ret_null */
  s[i] = arm_branch((uintptr_t)&s[i], resume, 0xea000000u); i++;   /* b resume */

  __builtin___clear_cache((char *)stub_addr, (char *)stub_addr + i * 4);
  *(uint32_t *)patch = arm_branch(patch, stub_addr, 0xea000000u);
  __builtin___clear_cache((char *)patch, (char *)patch + 4);
  fprintf(stderr, "installed Accelerometer null guard patch=%p stub=%p\n",
          (void *)patch, (void *)stub_addr);
}

static void install_accelerometer_device_null_guards(void) {
  if (!g_load_base) return;

  {
    uintptr_t patch = g_load_base + 0x36fa54u;
    uintptr_t stub_addr = g_load_base + 0x4f0200u;
    uintptr_t ret = g_load_base + 0x36fa34u;
    uintptr_t resume = g_load_base + 0x36fa5cu;
    uint32_t *s = (uint32_t *)stub_addr;
    int i = 0;

    s[i++] = 0xe3500000u; /* cmp r0, #0 */
    s[i] = arm_branch((uintptr_t)&s[i], ret, 0x0a000000u); i++; /* beq ret */
    s[i++] = 0xee181a10u; /* vmov r1, s16 */
    s[i++] = 0xe5903000u; /* ldr r3, [r0] */
    s[i] = arm_branch((uintptr_t)&s[i], resume, 0xea000000u); i++;

    __builtin___clear_cache((char *)stub_addr, (char *)stub_addr + i * 4);
    *(uint32_t *)patch = arm_branch(patch, stub_addr, 0xea000000u);
    __builtin___clear_cache((char *)patch, (char *)patch + 4);
    fprintf(stderr, "installed AccelerometerDevice setFrequency guard patch=%p stub=%p\n",
            (void *)patch, (void *)stub_addr);
  }

  {
    uintptr_t patch = g_load_base + 0x36f9fcu;
    uintptr_t stub_addr = g_load_base + 0x4f0240u;
    uintptr_t ret = g_load_base + 0x36fa08u;
    uintptr_t resume = g_load_base + 0x36fa00u;
    uint32_t *s = (uint32_t *)stub_addr;
    int i = 0;

    s[i++] = 0xe3500000u; /* cmp r0, #0 */
    s[i] = arm_branch((uintptr_t)&s[i], ret, 0x0a000000u); i++; /* beq ret */
    s[i++] = 0xe5903000u; /* ldr r3, [r0] */
    s[i] = arm_branch((uintptr_t)&s[i], resume, 0xea000000u); i++;

    __builtin___clear_cache((char *)stub_addr, (char *)stub_addr + i * 4);
    *(uint32_t *)patch = arm_branch(patch, stub_addr, 0xea000000u);
    __builtin___clear_cache((char *)patch, (char *)patch + 4);
    fprintf(stderr, "installed AccelerometerDevice getFrequency guard patch=%p stub=%p\n",
            (void *)patch, (void *)stub_addr);
  }
}

static int vfs_mount_path(const char *mount_point, const char *fs_path) {
  if (!g_load_base || !mount_point || !fs_path) return 0;

  void *(*vfs_get)(void) = (void *)(g_load_base + 0x35df54u);
  void (*vfs_mount)(void *, void *, void *, void *) = (void *)(g_load_base + 0x35ebdcu);

  GameWString mount_s;
  GameWString path_s;
  if (!game_wstring_init(&mount_s, mount_point) || !game_wstring_init(&path_s, fs_path)) {
    fprintf(stderr, "VFS mount string init failed: %s -> %s\n", mount_point, fs_path);
    return 0;
  }

  uintptr_t fs_shared[2];
  fs_shared[0] = (uintptr_t)g_posix_fs_obj;
  fs_shared[1] = (uintptr_t)g_posix_ref;

  void *vfs = vfs_get();
  if (!vfs) {
    fprintf(stderr, "VFS getVFS returned null\n");
    return 0;
  }

  vfs_mount(vfs, fs_shared, &mount_s, &path_s);
  fprintf(stderr, "VFS mount %s -> %s\n", mount_point, fs_path);
  return 1;
}

static void install_vfs_asset_mounts(void) {
  if (!g_load_base) return;

  const char *assets = getenv("DEADSPACE_ASSETS");
  if (!assets || !*assets) assets = "assets";

  ensure_posix_fs_bridge();

  char published[1024];
  snprintf(published, sizeof(published), "%s/published", assets);
  vfs_mount_path("published", published);
}

static void find_exports(void) {
#define FIND_REQ(var, sym)                                                     \
  do {                                                                         \
    var = (void *)so_find_addr_safe(sym);                                      \
    if (!var) fprintf(stderr, "WARN missing export %s\n", sym);               \
  } while (0)
  FIND_REQ(p_JNI_OnLoad, "JNI_OnLoad");
  FIND_REQ(p_EAIO_Startup, "Java_com_ea_EAIO_EAIO_Startup");
  FIND_REQ(p_rwfs_Startup, "Java_com_ea_rwfilesystem_rwfilesystem_Startup");
  FIND_REQ(p_Audio_Init, "Java_com_ea_EAAudioCore_AndroidEAAudioCore_Init");
  FIND_REQ(p_runEntryPoint, "Java_com_ea_DeadSpace_DeadSpace_runEntryPoint");
  FIND_REQ(p_NativeOnCreate, "Java_com_ea_blast_MainActivity_NativeOnCreate");
  FIND_REQ(p_NativeOnResume, "Java_com_ea_blast_MainActivity_NativeOnResume");
  FIND_REQ(p_NativeOnPause, "Java_com_ea_blast_MainActivity_NativeOnPause");
  FIND_REQ(p_NativeOnWindowFocusChanged, "Java_com_ea_blast_MainActivity_NativeOnWindowFocusChanged");
  FIND_REQ(p_SurfaceCreated, "Java_com_ea_blast_AndroidRenderer_NativeOnSurfaceCreated");
  FIND_REQ(p_SurfaceChanged, "Java_com_ea_blast_AndroidRenderer_NativeOnSurfaceChanged");
  FIND_REQ(p_DrawFrame, "Java_com_ea_blast_AndroidRenderer_NativeOnDrawFrame");
  FIND_REQ(p_KeyDown, "Java_com_ea_blast_KeyboardAndroid_NativeOnKeyDown");
  FIND_REQ(p_KeyUp, "Java_com_ea_blast_KeyboardAndroid_NativeOnKeyUp");
  FIND_REQ(p_PointerEvent, "Java_com_ea_blast_TouchSurfaceAndroid_NativeOnPointerEvent");
  FIND_REQ(p_ModPhysicalKeyboard, "Java_com_ea_blast_ModuleCatalog_NativeGetModuleTypeIdPhysicalKeyboard");
  FIND_REQ(p_ModTouchScreen, "Java_com_ea_blast_ModuleCatalog_NativeGetModuleTypeIdTouchScreen");
  FIND_REQ(p_ModTouchPad, "Java_com_ea_blast_ModuleCatalog_NativeGetModuleTypeIdTouchPad");
  FIND_REQ(p_RawDown, "Java_com_ea_blast_TouchSurfaceAndroid_NativeGetIdRawPointerDown");
  FIND_REQ(p_RawMove, "Java_com_ea_blast_TouchSurfaceAndroid_NativeGetIdRawPointerMove");
  FIND_REQ(p_RawUp, "Java_com_ea_blast_TouchSurfaceAndroid_NativeGetIdRawPointerUp");
  FIND_REQ(p_RawCancel, "Java_com_ea_blast_TouchSurfaceAndroid_NativeGetIdRawPointerCancel");
#undef FIND_REQ
}

static void send_key(int key, int down) {
  if (!g_mod_key) return;
  if (control_log_enabled()) {
    static int log_count;
    if (log_count < 256) {
      fprintf(stderr, "[ctl] key %d %s\n", key, down ? "down" : "up");
      log_count++;
    }
  }
  if (down) {
    if (p_KeyDown) p_KeyDown(g_env, NULL, g_mod_key, key, 0);
  } else {
    if (p_KeyUp) p_KeyUp(g_env, NULL, g_mod_key, key, 0);
  }
}

static const char *touch_module_name(int module_id) {
  if (module_id && module_id == g_mod_touchpad) return "touchpad";
  if (module_id && module_id == g_mod_touch) return "touchscreen";
  return "touch";
}

static void send_touch_raw_module(int module_id, int id, int raw, float x, float y) {
  if (control_log_enabled()) {
    static int log_count;
    if (log_count < 384) {
      fprintf(stderr, "[ctl] %s id=%d raw=%d %.1f,%.1f\n",
              touch_module_name(module_id), id, raw, x, y);
      log_count++;
    }
  }
  if (p_PointerEvent && module_id && raw)
    p_PointerEvent(g_env, NULL, raw, module_id, id, x, y);
}

static void send_touch_raw(int id, int raw, float x, float y) {
  send_touch_raw_module(g_mod_touch, id, raw, x, y);
}

static void touch_down(int id, float x, float y) { send_touch_raw(id, g_raw_down, x, y); }
static void touch_move(int id, float x, float y) { send_touch_raw(id, g_raw_move, x, y); }
static void touch_up(int id, float x, float y) { send_touch_raw(id, g_raw_up, x, y); }
static void touch_down_module(int module_id, int id, float x, float y) {
  send_touch_raw_module(module_id, id, g_raw_down, x, y);
}
static void touch_move_module(int module_id, int id, float x, float y) {
  send_touch_raw_module(module_id, id, g_raw_move, x, y);
}
static void touch_up_module(int module_id, int id, float x, float y) {
  send_touch_raw_module(module_id, id, g_raw_up, x, y);
}

static float sx(float v) { return v * (float)g_w / 1280.0f; }
static float sy(float v) { return v * (float)g_h / 720.0f; }

typedef struct {
  int active;
  int module_id;
  float x, y;
} TouchSlot;
static TouchSlot g_touch[16];

static void refresh_touch_slot(int id) {
  if (id < 0 || id >= (int)(sizeof(g_touch) / sizeof(g_touch[0]))) return;
  if (g_touch[id].active)
    touch_move_module(g_touch[id].module_id, id, g_touch[id].x, g_touch[id].y);
}

static void set_button_touch_module(int module_id, int id, int pressed, float x, float y) {
  if (id < 0 || id >= (int)(sizeof(g_touch) / sizeof(g_touch[0]))) return;
  if (!module_id) module_id = g_mod_touch;
  x = sx(x);
  y = sy(y);
  if (pressed) {
    if (g_touch[id].active && g_touch[id].module_id != module_id)
      touch_up_module(g_touch[id].module_id, id, g_touch[id].x, g_touch[id].y);
    if (!g_touch[id].active || g_touch[id].module_id != module_id)
      touch_down_module(module_id, id, x, y);
    else
      touch_move_module(module_id, id, x, y);
    g_touch[id].active = 1;
    g_touch[id].module_id = module_id;
    g_touch[id].x = x;
    g_touch[id].y = y;
  } else if (g_touch[id].active) {
    touch_up_module(g_touch[id].module_id, id, g_touch[id].x, g_touch[id].y);
    g_touch[id].active = 0;
    g_touch[id].module_id = 0;
  }
}

static void set_button_touch(int id, int pressed, float x, float y) {
  set_button_touch_module(g_mod_touch, id, pressed, x, y);
}

static int d_up, d_down, d_left, d_right, r_up, r_down, r_left, r_right, lx, ly, rx, ry;
static int l2_pressed, r2_pressed;
static int left_release_pending, right_release_pending;
static Uint32 left_release_at, right_release_at;
static int right_swipe_active;
static Uint32 right_swipe_release_at, right_swipe_next_at;

static float clampf_local(float v, float lo, float hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static float axis_norm(int v) {
  if (v < -32767) v = -32767;
  if (v > 32767) v = 32767;
  return (float)v / 32767.0f;
}

static void set_drag_touch_module(int module_id, int id, float base_x, float base_y,
                                  float x, float y, int *release_pending) {
  if (!g_touch[id].active) set_button_touch_module(module_id, id, 1, base_x, base_y);
  set_button_touch_module(module_id, id, 1, x, y);
  if (release_pending) *release_pending = 0;
}

static int right_touch_module(void) {
  if (env_flag("DS_RIGHT_TOUCHPAD") && g_mod_touchpad) return g_mod_touchpad;
  return g_mod_touch;
}

static void release_right_touch(void) {
  if (g_touch[5].active) set_button_touch_module(g_touch[5].module_id, 5, 0, 0, 0);
  right_release_pending = 0;
  right_swipe_active = 0;
}

static void request_drag_release(int id, float base_x, float base_y,
                                 int *release_pending, Uint32 *release_at) {
  if (!g_touch[id].active) {
    if (release_pending) *release_pending = 0;
    return;
  }
  if (release_pending && !*release_pending) {
    *release_pending = 1;
    if (release_at) *release_at = SDL_GetTicks() + 140;
  }
  (void)base_x;
  (void)base_y;
}

static void expire_drag_releases(void) {
  Uint32 now = SDL_GetTicks();
  if (left_release_pending && SDL_TICKS_PASSED(now, left_release_at)) {
    set_button_touch(0, 0, 360, 440);
    left_release_pending = 0;
  }
  if (right_release_pending && SDL_TICKS_PASSED(now, right_release_at)) {
    release_right_touch();
    right_release_pending = 0;
  }
}

static void update_left_touch(void) {
  const float base_x = 360.0f, base_y = 440.0f;
  float vx = (d_right ? 1.0f : 0.0f) - (d_left ? 1.0f : 0.0f);
  float vy = (d_down ? 1.0f : 0.0f) - (d_up ? 1.0f : 0.0f);
  float mag = sqrtf(vx * vx + vy * vy);
  float scale = 1.0f;

  if (mag <= 0.0f) {
    vx = axis_norm(lx);
    vy = axis_norm(ly);
    mag = sqrtf(vx * vx + vy * vy);
    if (mag < 0.12f) {
      request_drag_release(0, base_x, base_y, &left_release_pending, &left_release_at);
      return;
    }
    if (mag < 0.18f && !g_touch[0].active) return;
    scale = clampf_local((mag - 0.12f) / 0.88f, 0.62f, 1.0f);
  }

  if (mag > 1.0f) {
    vx /= mag;
    vy /= mag;
  } else if (mag > 0.0f) {
    vx /= mag;
    vy /= mag;
  } else {
    return;
  }

  float x = base_x + vx * (190.0f * scale);
  float y = base_y + vy * (220.0f * scale);
  set_drag_touch_module(g_mod_touch, 0, base_x, base_y, x, y, &left_release_pending);
}

static void update_right_touch(void) {
  const float base_x = 960.0f, base_y = 360.0f;
  float vx = (r_right ? 1.0f : 0.0f) - (r_left ? 1.0f : 0.0f);
  float vy = (r_down ? 1.0f : 0.0f) - (r_up ? 1.0f : 0.0f);
  float mag = sqrtf(vx * vx + vy * vy);
  float scale = 1.0f;

  if (mag <= 0.0f) {
    vx = axis_norm(rx);
    vy = axis_norm(ry);
    mag = sqrtf(vx * vx + vy * vy);
    if (mag < 0.10f) {
      release_right_touch();
      return;
    }
    if (mag < 0.16f && !g_touch[5].active) return;
    scale = clampf_local((mag - 0.10f) / 0.90f, 0.45f, 1.0f);
  }

  if (mag > 1.0f) {
    vx /= mag;
    vy /= mag;
  } else if (mag > 0.0f) {
    vx /= mag;
    vy /= mag;
  } else {
    return;
  }

  if (env_flag("DS_RIGHT_HOLD") || env_flag("DS_RIGHT_TOUCHPAD")) {
    float x = base_x + vx * (180.0f * scale);
    float y = base_y + vy * (140.0f * scale);
    right_swipe_active = 0;
    set_drag_touch_module(right_touch_module(), 5, base_x, base_y, x, y, &right_release_pending);
    return;
  }

  int module_id = right_touch_module();
  if (!module_id) return;

  Uint32 now = SDL_GetTicks();
  int pulse_ms = env_int("DS_RIGHT_PULSE_MS", 26);
  int gap_ms = env_int("DS_RIGHT_GAP_MS", 6);
  int amp_x = env_int("DS_RIGHT_AMP_X", 150);
  int amp_y = env_int("DS_RIGHT_AMP_Y", 96);
  if (pulse_ms < 8) pulse_ms = 8;
  if (gap_ms < 0) gap_ms = 0;
  if (amp_x < 20) amp_x = 20;
  if (amp_y < 20) amp_y = 20;

  float x = base_x + vx * ((float)amp_x * scale);
  float y = base_y + vy * ((float)amp_y * scale);
  right_release_pending = 0;

  if (right_swipe_active) {
    set_button_touch_module(module_id, 5, 1, x, y);
    if (SDL_TICKS_PASSED(now, right_swipe_release_at)) {
      set_button_touch_module(module_id, 5, 0, x, y);
      right_swipe_active = 0;
      right_swipe_next_at = now + (Uint32)gap_ms;
    }
    return;
  }

  if (!SDL_TICKS_PASSED(now, right_swipe_next_at)) return;
  if (g_touch[5].active) set_button_touch_module(module_id, 5, 0, x, y);
  set_button_touch_module(module_id, 5, 1, base_x, base_y);
  set_button_touch_module(module_id, 5, 1, x, y);
  right_swipe_active = 1;
  right_swipe_release_at = now + (Uint32)pulse_ms;
  right_swipe_next_at = right_swipe_release_at + (Uint32)gap_ms;
}

/* ======================================================================
 * Rota nativa de gamepad (injecao direta no InputSchemeDPadsRel do jogo)
 *
 * O jogo (EA Blast/ironmonkey) processa gameplay assim:
 *   Hud (LayerGameWorld+0x260) -> InputSchemeDPadsRel (hud+0x10)
 *     +0xd4 shared_ptr<InputForwarderTouchDPad> fwdMove  (dpad esquerdo)
 *     +0xe0 shared_ptr<InputForwarderTouchDPad> fwdLook  (dpad direito)
 *   InputForwarderTouchDPad::sendDPadEvent(this, float dx, float dy)
 *     normaliza (dx,dy) em pixels, calcula flag de corrida e despacha o
 *     evento 1006 que o player/camera consomem.
 *   Hud::doSpecialAction(this, 0x352fb91+idx, param) executa as 16 acoes
 *     nativas (aim/fire/melee/reload/stasis/kinesis/arma/pause/back...).
 *
 * Hookamos InputSchemeDPadsRel::onUpdateEvent (roda por frame na thread
 * do jogo) para injetar analogicos e drenar a fila de botoes com
 * seguranca de thread. Menus continuam na rota antiga de toque/tecla.
 * ====================================================================== */
#define DS_OFF_DPADSREL_ONUPDATE 0x81608u
#define DS_OFF_SENDDPADEVENT     0x7c614u
#define DS_OFF_TWEAKS_GET        0x22f7b8u
#define DS_OFF_HUD_DOSPECIAL     0x7541cu
#define DS_OFF_HUD_ISPAUSED      0x6e498u
#define DS_NATIVE_ACTION_BASE    0x0352fb91u

#define DS_SCHEME_OFF_GAMEWORLD 0x10
#define DS_SCHEME_OFF_FWD_MOVE  0xd4
#define DS_SCHEME_OFF_FWD_LOOK  0xe0
#define DS_FWD_OFF_ACTIVE       0x2c
#define DS_FWD_OFF_DEADZONE     0x30
#define DS_FWD_OFF_RUNTHRESH    0x3c
#define DS_GW_OFF_PLAYER        0xd4
#define DS_PLAYER_OFF_AIMING    0x1ed
#define DS_TWEAKS_OFF_DZ        0x178
#define DS_TWEAKS_OFF_DZ_AIM    0x17c

typedef void (SFCALL *ds_send_dpad_fn)(void *fwd, float x, float y);
typedef void (*ds_dospecial_fn)(void *hud, int action_id, int param);
typedef unsigned char (*ds_ispaused_fn)(const void *hud);
typedef void *(*ds_tweaks_get_fn)(void);

/* indices default das acoes nativas (0x352fb91 + idx) */
enum {
  DS_ACT_UNKNOWN0 = 0,
  DS_ACT_RIG = 1,          /* abrir inventario/RIG (StringIdEvent) */
  DS_ACT_WEAPON_NEXT = 2,
  DS_ACT_WEAPON_PREV = 3,
  DS_ACT_LOCATOR = 4,      /* linha ate o objetivo */
  DS_ACT_MELEE = 5,
  DS_ACT_STASIS = 6,       /* evento 1008 subtipo 10 */
  DS_ACT_AIM = 7,          /* setAiming(param) */
  DS_ACT_FIRE = 8,         /* evento 1008 subtipo 6, exige mira */
  DS_ACT_RELOAD = 9,       /* evento 1008 subtipo 8 */
  DS_ACT_TAP_CENTER = 10,  /* toque no centro da tela: porta/item/kinesis */
  DS_ACT_GRAPPLE = 11,     /* escapar de agarrao */
  DS_ACT_BACK = 12,        /* mesmo do keycode BACK */
  DS_ACT_UNKNOWN13 = 13,
  DS_ACT_PAUSE = 14,       /* mesmo do keycode MENU */
  DS_ACT_UNKNOWN15 = 15
};

static volatile float g_njoy_lx, g_njoy_ly, g_njoy_rx, g_njoy_ry;
static float g_auto_lx, g_auto_ly, g_auto_rx, g_auto_ry;  /* DS_AUTOINPUT */
static uint8_t g_polled_buttons[SDL_CONTROLLER_BUTTON_MAX];
/* modo cursor DENTRO do gameplay (Select liga/desliga) para objetos
 * interativos touch (alavancas, paineis, minigames) */
static volatile int g_gp_cursor_mode;
static volatile Uint32 g_native_seen_ms;
static volatile int g_native_paused = 1;
static int g_native_hook_installed;
static int g_cfg_move_amp, g_cfg_look_amp, g_cfg_look_scale, g_cfg_native_log;

#define DS_NATQ_SIZE 64
static volatile uint32_t g_natq[DS_NATQ_SIZE];
static volatile unsigned g_natq_w, g_natq_r;

static void native_queue_action(int idx, int param) {
  unsigned w = g_natq_w;
  if (idx < 0 || idx > 255) return;
  if (w - g_natq_r >= DS_NATQ_SIZE) return;
  g_natq[w % DS_NATQ_SIZE] = ((uint32_t)idx << 8) | (uint32_t)(param & 0xff);
  __sync_synchronize();
  g_natq_w = w + 1;
}

static int native_gameplay_active(void) {
  if (!g_native_hook_installed) return 0;
  Uint32 seen = g_native_seen_ms;
  if (!seen) return 0;
  if (SDL_GetTicks() - seen > 400) return 0;
  return !g_native_paused;
}

/* roda NA THREAD DO JOGO no lugar de InputSchemeDPadsRel::onUpdateEvent */
static int ds_native_onupdate_hook(void *scheme, void *upd) {
  (void)upd;
  void *gw = *(void **)((char *)scheme + DS_SCHEME_OFF_GAMEWORLD);
  void *fwd_move = *(void **)((char *)scheme + DS_SCHEME_OFF_FWD_MOVE);
  void *fwd_look = *(void **)((char *)scheme + DS_SCHEME_OFF_FWD_LOOK);
  static int move_on, look_on;

  /* logica original da funcao: atualizar deadzone via Tweaks */
  if (gw) {
    void *player = *(void **)((char *)gw + DS_GW_OFF_PLAYER);
    if (player && fwd_move) {
      char *tw = (char *)((ds_tweaks_get_fn)(g_load_base + DS_OFF_TWEAKS_GET))();
      int aiming = *(unsigned char *)((char *)player + DS_PLAYER_OFF_AIMING);
      *(int *)((char *)fwd_move + DS_FWD_OFF_DEADZONE) =
          *(int *)(tw + (aiming ? DS_TWEAKS_OFF_DZ_AIM : DS_TWEAKS_OFF_DZ));
    }
  }

  void *hud = (char *)scheme - 0x10;
  int paused = 1;
  if (gw)
    paused = ((ds_ispaused_fn)(g_load_base + DS_OFF_HUD_ISPAUSED))(hud) ? 1 : 0;
  g_native_paused = paused;
  g_native_seen_ms = SDL_GetTicks();

  ds_send_dpad_fn send_dpad = (ds_send_dpad_fn)(g_load_base + DS_OFF_SENDDPADEVENT);

  if (paused || !gw) {
    /* solta os dois dpads para nao ficar andando/girando no pause */
    if (move_on && fwd_move) {
      send_dpad(fwd_move, 0.0f, 0.0f);
      *(signed char *)((char *)fwd_move + DS_FWD_OFF_ACTIVE) = 0;
      move_on = 0;
    }
    if (look_on && fwd_look) {
      send_dpad(fwd_look, 0.0f, 0.0f);
      *(signed char *)((char *)fwd_look + DS_FWD_OFF_ACTIVE) = 0;
      look_on = 0;
    }
    g_natq_r = g_natq_w;  /* descarta botoes pendentes */
    return 0;
  }

  if (fwd_move) {
    /* no modo cursor o stick esquerdo move o cursor, nao o Isaac */
    float x = g_gp_cursor_mode ? 0.0f : g_njoy_lx;
    float y = g_gp_cursor_mode ? 0.0f : g_njoy_ly;
    float m = sqrtf(x * x + y * y);
    if (m > 0.02f) {
      int dz = *(int *)((char *)fwd_move + DS_FWD_OFF_DEADZONE);
      int rt = *(int *)((char *)fwd_move + DS_FWD_OFF_RUNTHRESH);
      float amp = (float)g_cfg_move_amp;
      if (amp <= 0.0f) {
        amp = 1.35f * (float)(dz > rt ? dz : rt);
        if (amp < 70.0f) amp = 70.0f;
      }
      *(signed char *)((char *)fwd_move + DS_FWD_OFF_ACTIVE) = 1;
      send_dpad(fwd_move, x * amp, y * amp);
      move_on = 1;
    } else if (move_on) {
      send_dpad(fwd_move, 0.0f, 0.0f);
      *(signed char *)((char *)fwd_move + DS_FWD_OFF_ACTIVE) = 0;
      move_on = 0;
    }
  }

  if (fwd_look) {
    /* Formula real do consumidor (GameObjectPlayable::onEvent, dpad 3):
     *   delta = evento.xy * Tweaks[0x54/0x58] * Settings.sensitivity
     * Linear, por evento, SEM saturacao — mas a deadzone do forwarder
     * ([fwd+0x30]) zera magnitudes baixas. Entao: magnitude enviada =
     * deadzone + DS_LOOK_AMP * deflexao (sempre logo acima da deadzone,
     * velocidade linear e controlavel de verdade). */
    float x = g_njoy_rx, y = g_njoy_ry;
    float m = sqrtf(x * x + y * y);
    if (m > 0.02f) {
      if (m > 1.0f) m = 1.0f;
      /* Escala real do consumidor: 1px/frame = giro bem lento (v1),
       * ~10px = rapido. DS_LOOK_AMP 1..100 mapeia esse intervalo. */
      float amp = (float)g_cfg_look_amp;
      if (amp <= 0.0f) amp = 35.0f;
      float px = 1.0f + (amp / 100.0f) * 9.0f * m * m;
      /* zera a deadzone do forwarder de camera para o jogo aceitar
       * magnitudes pequenas (o jogo so re-escreve a do movimento) */
      *(int *)((char *)fwd_look + DS_FWD_OFF_DEADZONE) = 0;
      send_dpad(fwd_look, (x / m) * px, (y / m) * px);
      look_on = 1;
    } else if (look_on) {
      send_dpad(fwd_look, 0.0f, 0.0f);
      look_on = 0;
    }
  }

  while (g_natq_r != g_natq_w) {
    uint32_t v = g_natq[g_natq_r % DS_NATQ_SIZE];
    __sync_synchronize();
    g_natq_r++;
    int idx = (int)((v >> 8) & 0xff);
    int param = (int)(v & 0xff);
    if (g_cfg_native_log) {
      static int nlog;
      if (nlog < 200) {
        fprintf(stderr, "[native] doSpecialAction idx=%d param=%d\n", idx, param);
        nlog++;
      }
    }
    ((ds_dospecial_fn)(g_load_base + DS_OFF_HUD_DOSPECIAL))(
        hud, (int)(DS_NATIVE_ACTION_BASE + (uint32_t)idx), param);
  }
  return 0;
}

static void install_native_input_hook(void) {
  if (!g_load_base) return;
  if (env_flag("DS_NO_NATIVE")) {
    fprintf(stderr, "native input hook desligado (DS_NO_NATIVE)\n");
    return;
  }
  g_cfg_move_amp = env_int("DS_MOVE_AMP", 0);
  g_cfg_look_amp = env_int("DS_LOOK_AMP", 0);
  g_cfg_look_scale = env_int("DS_LOOK_SCALE", 55);
  g_cfg_native_log = env_flag("DS_NATIVELOG") || control_log_enabled();
  uintptr_t stub = g_load_base + 0x4f0700u;
  write_abs_jump_stub(stub, (uintptr_t)&ds_native_onupdate_hook);
  patch_arm_entry(g_load_base + DS_OFF_DPADSREL_ONUPDATE, stub);
  g_native_hook_installed = 1;
  fprintf(stderr, "native input hook instalado em 0x%x (stub %p)\n",
          DS_OFF_DPADSREL_ONUPDATE, (void *)stub);
}

/* mapeamento botao SDL -> acao nativa; mode 1 = hold (manda 1 no press e
 * 0 no release), mode 0 = tap (manda so no press com param 0) */
typedef struct {
  const char *env;
  int idx;
  int hold;
} NativeBtnCfg;

enum {
  NBTN_A, NBTN_B, NBTN_X, NBTN_Y, NBTN_LB, NBTN_RB, NBTN_LT, NBTN_RT,
  NBTN_START, NBTN_BACK, NBTN_L3, NBTN_R3, NBTN_COUNT
};

static NativeBtnCfg g_nbtn[NBTN_COUNT] = {
  [NBTN_A]     = {"DS_BTN_A",     DS_ACT_TAP_CENTER, 0},  /* abre porta/pega/usa */
  [NBTN_B]     = {"DS_BTN_B",     DS_ACT_MELEE,      0},
  [NBTN_X]     = {"DS_BTN_X",     DS_ACT_RELOAD,     0},
  [NBTN_Y]     = {"DS_BTN_Y",     DS_ACT_WEAPON_NEXT, 0},
  [NBTN_LB]    = {"DS_BTN_LB",    DS_ACT_STASIS,     0},
  [NBTN_RB]    = {"DS_BTN_RB",    DS_ACT_RIG,        0},  /* inventario */
  [NBTN_LT]    = {"DS_BTN_LT",    DS_ACT_AIM,        1},
  [NBTN_RT]    = {"DS_BTN_RT",    DS_ACT_FIRE,       0},
  [NBTN_START] = {"DS_BTN_START", DS_ACT_BACK,       0},  /* pause real */
  [NBTN_BACK]  = {"DS_BTN_BACK",  -1,                0},  /* Select = so cursor */
  [NBTN_L3]    = {"DS_BTN_L3",    -1,                0},
  [NBTN_R3]    = {"DS_BTN_R3",    DS_ACT_LOCATOR,    0},
};

static void native_btn_load_env(void) {
  for (int i = 0; i < NBTN_COUNT; i++) {
    const char *v = getenv(g_nbtn[i].env);
    if (!v || !*v) continue;
    g_nbtn[i].idx = atoi(v);
    g_nbtn[i].hold = strchr(v, 'h') != NULL;
    if (g_nbtn[i].idx > 15) g_nbtn[i].idx = -1;
  }
}

static void native_button(int nbtn, int down) {
  if (nbtn < 0 || nbtn >= NBTN_COUNT) return;
  int idx = g_nbtn[nbtn].idx;
  if (idx < 0) return;
  if (g_nbtn[nbtn].hold) {
    native_queue_action(idx, down ? 1 : 0);
  } else if (down) {
    native_queue_action(idx, 0);
  }
}

static int native_btn_for_controller(SDL_GameControllerButton b) {
  switch (b) {
    case SDL_CONTROLLER_BUTTON_A: return NBTN_A;
    case SDL_CONTROLLER_BUTTON_B: return NBTN_B;
    case SDL_CONTROLLER_BUTTON_X: return NBTN_X;
    case SDL_CONTROLLER_BUTTON_Y: return NBTN_Y;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return NBTN_LB;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return NBTN_RB;
    case SDL_CONTROLLER_BUTTON_START: return NBTN_START;
    case SDL_CONTROLLER_BUTTON_BACK: return NBTN_BACK;
    case SDL_CONTROLLER_BUTTON_LEFTSTICK: return NBTN_L3;
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return NBTN_R3;
    default: return -1;
  }
}

static int key_for_button(SDL_GameControllerButton b) {
  switch (b) {
    case SDL_CONTROLLER_BUTTON_A: return AK_BUTTON_A;
    case SDL_CONTROLLER_BUTTON_B: return AK_BUTTON_B;
    case SDL_CONTROLLER_BUTTON_X: return AK_BUTTON_X;
    case SDL_CONTROLLER_BUTTON_Y: return AK_BUTTON_Y;
    case SDL_CONTROLLER_BUTTON_GUIDE: return AK_BUTTON_MODE;
    case SDL_CONTROLLER_BUTTON_BACK: return AK_BUTTON_SELECT;
    case SDL_CONTROLLER_BUTTON_START: return AK_BUTTON_START;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return AK_BUTTON_L1;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return AK_BUTTON_R1;
    case SDL_CONTROLLER_BUTTON_LEFTSTICK: return AK_BUTTON_THUMBL;
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return AK_BUTTON_THUMBR;
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return AK_DPAD_UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return AK_DPAD_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return AK_DPAD_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return AK_DPAD_RIGHT;
    default: return 0;
  }
}

static int button_touch_enabled(void) {
  return env_flag("DS_BUTTON_TOUCH");
}

static int menu_touch_enabled(void) {
  /* taps cegos de menu agora sao opt-in; o cursor virtual e a rota padrao */
  return env_flag("DS_MENU_TAPS");
}

/* ---- cursor virtual para menus touch ---- */
static float g_cur_x = 640.0f, g_cur_y = 360.0f;
static int g_cursor_visible;
static int g_cursor_pressed;

static int menu_cursor_enabled(void) {
  return !env_flag("DS_NO_CURSOR");
}

static void cursor_move(float dx, float dy) {
  g_cur_x = clampf_local(g_cur_x + dx, 8.0f, 1272.0f);
  g_cur_y = clampf_local(g_cur_y + dy, 8.0f, 712.0f);
}

static void cursor_press(int down) {
  if (down) {
    touch_down(12, sx(g_cur_x), sy(g_cur_y));
    g_cursor_pressed = 1;
  } else if (g_cursor_pressed) {
    touch_up(12, sx(g_cur_x), sy(g_cur_y));
    g_cursor_pressed = 0;
  }
}

static void cursor_update_from_pad(void) {
  if (!menu_cursor_enabled()) return;
  if (!native_gameplay_active()) g_gp_cursor_mode = 0;
  if (native_gameplay_active() && !g_gp_cursor_mode) {
    g_cursor_visible = 0;
    if (g_cursor_pressed) cursor_press(0);
    return;
  }
  if (g_gp_cursor_mode) g_cursor_visible = 1;
  float vx = axis_norm(lx), vy = axis_norm(ly);
  float m = sqrtf(vx * vx + vy * vy);
  if (m < 0.2f) { vx = 0.0f; vy = 0.0f; }
  if (vx == 0.0f && vy == 0.0f) {
    vx = (g_polled_buttons[SDL_CONTROLLER_BUTTON_DPAD_RIGHT] ? 1.0f : 0.0f) -
         (g_polled_buttons[SDL_CONTROLLER_BUTTON_DPAD_LEFT] ? 1.0f : 0.0f);
    vy = (g_polled_buttons[SDL_CONTROLLER_BUTTON_DPAD_DOWN] ? 1.0f : 0.0f) -
         (g_polled_buttons[SDL_CONTROLLER_BUTTON_DPAD_UP] ? 1.0f : 0.0f);
    vx *= 0.75f;
    vy *= 0.75f;
  }
  if (vx != 0.0f || vy != 0.0f) {
    float speed = (float)env_int("DS_CURSOR_SPEED", 11);
    cursor_move(vx * speed, vy * speed);
    g_cursor_visible = 1;
    if (g_cursor_pressed) touch_move(12, sx(g_cur_x), sy(g_cur_y));
  }
}

static void draw_menu_cursor(void) {
  if (env_flag("DS_CURSOR_TEST")) g_cursor_visible = 1;
  if (!g_cursor_visible || !menu_cursor_enabled()) return;
  if (native_gameplay_active() && !g_gp_cursor_mode) return;

  GLint vp[4];
  glGetIntegerv(GL_VIEWPORT, vp);
  glViewport(0, 0, g_w, g_h);
  glMatrixMode(GL_PROJECTION);
  glPushMatrix();
  glLoadIdentity();
  glOrthof(0.0f, 1280.0f, 720.0f, 0.0f, -1.0f, 1.0f);
  glMatrixMode(GL_MODELVIEW);
  glPushMatrix();
  glLoadIdentity();

  glDisable(GL_DEPTH_TEST);
  glDisable(GL_TEXTURE_2D);
  glDisable(GL_LIGHTING);
  glDisable(GL_CULL_FACE);
  glDisable(GL_ALPHA_TEST);
  glDisable(GL_SCISSOR_TEST);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glDisableClientState(GL_COLOR_ARRAY);
  glDisableClientState(GL_TEXTURE_COORD_ARRAY);
  glEnableClientState(GL_VERTEX_ARRAY);

  float x = g_cur_x, y = g_cur_y;
  const float L = 14.0f, T = 2.5f;
  GLfloat v[] = {
    /* barra horizontal */
    x - L, y - T, x + L, y - T, x + L, y + T,
    x - L, y - T, x + L, y + T, x - L, y + T,
    /* barra vertical */
    x - T, y - L, x + T, y - L, x + T, y + L,
    x - T, y - L, x + T, y + L, x - T, y + L,
  };
  glVertexPointer(2, GL_FLOAT, 0, v);
  glColor4f(0.15f, 0.05f, 0.05f, 0.85f);
  glDrawArrays(GL_TRIANGLES, 0, 12);
  GLfloat v2[24];
  for (int i = 0; i < 24; i += 2) { v2[i] = v[i] * 0.999f + x * 0.001f; v2[i + 1] = v[i + 1] * 0.999f + y * 0.001f; }
  glColor4f(0.3f, 1.0f, 1.0f, 0.95f);
  glVertexPointer(2, GL_FLOAT, 0, v2);
  glDrawArrays(GL_TRIANGLES, 0, 12);

  glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
  glMatrixMode(GL_PROJECTION);
  glPopMatrix();
  glMatrixMode(GL_MODELVIEW);
  glPopMatrix();
  glViewport(vp[0], vp[1], vp[2], vp[3]);
}

static void menu_touch(int id, int pressed, int fallback_x, int fallback_y) {
  if (!menu_touch_enabled()) return;
  int x = env_int("DS_MENU_TOUCH_X", fallback_x);
  int y = fallback_y;
  if (id == 12) y = env_int("DS_MENU_CONFIRM_Y", fallback_y);
  else if (id == 13) y = env_int("DS_MENU_UP_Y", fallback_y);
  else if (id == 14) y = env_int("DS_MENU_DOWN_Y", fallback_y);
  set_button_touch(id, pressed, x, y);
}

static void send_trigger_state(int key, int touch_id, float x, float y, int pressed, int *state) {
  if (pressed == *state) return;
  *state = pressed;
  if (native_gameplay_active()) {
    native_button(key == AK_BUTTON_L2 ? NBTN_LT : NBTN_RT, pressed);
    return;
  }
  send_key(key, pressed);
  if (button_touch_enabled()) set_button_touch(touch_id, pressed, x, y);
}

static void update_trigger_axis(int key, int touch_id, float x, float y, int value, int *state) {
  if (value > 16000) send_trigger_state(key, touch_id, x, y, 1, state);
  else if (value < 10000) send_trigger_state(key, touch_id, x, y, 0, state);
}

static void touch_for_button(SDL_GameControllerButton b, int pressed) {
  switch (b) {
    case SDL_CONTROLLER_BUTTON_A: set_button_touch(1, pressed, 1110, 615); break;
    case SDL_CONTROLLER_BUTTON_B: set_button_touch(2, pressed, 1210, 515); break;
    case SDL_CONTROLLER_BUTTON_X: set_button_touch(3, pressed, 1010, 515); break;
    case SDL_CONTROLLER_BUTTON_Y: set_button_touch(4, pressed, 1110, 390); break;
    case SDL_CONTROLLER_BUTTON_START: set_button_touch(6, pressed, 640, 675); break;
    case SDL_CONTROLLER_BUTTON_BACK: set_button_touch(7, pressed, 70, 55); break;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: set_button_touch(8, pressed, 930, 625); break;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: set_button_touch(9, pressed, 1210, 625); break;
    default: break;
  }
}

static void handle_controller_button(SDL_GameControllerButton b, int down) {
  if (native_gameplay_active()) {
    /* Select: liga/desliga o cursor de toque dentro do gameplay */
    if (b == SDL_CONTROLLER_BUTTON_BACK) {
      if (down) {
        g_gp_cursor_mode = !g_gp_cursor_mode;
        if (!g_gp_cursor_mode && g_cursor_pressed) cursor_press(0);
        if (control_log_enabled())
          fprintf(stderr, "[ctl] cursor gameplay %s\n", g_gp_cursor_mode ? "ON" : "OFF");
      }
      return;
    }
    /* com cursor ligado, A toca onde o cursor esta */
    if (g_gp_cursor_mode && b == SDL_CONTROLLER_BUTTON_A) {
      cursor_press(down);
      return;
    }
    /* gameplay: acoes nativas do jogo; dpad vira movimento (poll analogico) */
    int nbtn = native_btn_for_controller(b);
    if (nbtn >= 0) {
      native_button(nbtn, down);
      return;
    }
    if (b == SDL_CONTROLLER_BUTTON_DPAD_UP || b == SDL_CONTROLLER_BUTTON_DPAD_DOWN ||
        b == SDL_CONTROLLER_BUTTON_DPAD_LEFT || b == SDL_CONTROLLER_BUTTON_DPAD_RIGHT)
      return;
    if (b == SDL_CONTROLLER_BUTTON_GUIDE) return;
  }
  int key = key_for_button(b);
  if (key) send_key(key, down);
  if (b == SDL_CONTROLLER_BUTTON_A) {
    send_key(AK_ENTER, down);
    send_key(AK_DPAD_CENTER, down);
    if (menu_cursor_enabled()) cursor_press(down);
    else menu_touch(12, down, 640, 360);
  } else if (b == SDL_CONTROLLER_BUTTON_B) {
    /* em menus, BACK e tecla nativa (Application::OnKeyDown trata) */
    send_key(AK_BACK, down);
  } else if (b == SDL_CONTROLLER_BUTTON_START) {
    send_key(AK_ENTER, down);
    send_key(AK_DPAD_CENTER, down);
    menu_touch(12, down, 640, 360);
  } else if (b == SDL_CONTROLLER_BUTTON_DPAD_UP) {
    menu_touch(13, down, 640, 300);
  } else if (b == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
    menu_touch(14, down, 640, 420);
  } else if (b == SDL_CONTROLLER_BUTTON_DPAD_LEFT) {
    menu_touch(15, down, 500, 360);
  } else if (b == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) {
    menu_touch(15, down, 780, 360);
  }
  if (button_touch_enabled() &&
      b != SDL_CONTROLLER_BUTTON_DPAD_UP &&
      b != SDL_CONTROLLER_BUTTON_DPAD_DOWN &&
      b != SDL_CONTROLLER_BUTTON_DPAD_LEFT &&
      b != SDL_CONTROLLER_BUTTON_DPAD_RIGHT)
    touch_for_button(b, down);
}

static void handle_joy_button(int button, int down) {
  switch (button) {
    case 0: handle_controller_button(SDL_CONTROLLER_BUTTON_A, down); break;
    case 1: handle_controller_button(SDL_CONTROLLER_BUTTON_B, down); break;
    case 2: handle_controller_button(SDL_CONTROLLER_BUTTON_X, down); break;
    case 3: handle_controller_button(SDL_CONTROLLER_BUTTON_Y, down); break;
    case 4: handle_controller_button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, down); break;
    case 5: handle_controller_button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, down); break;
    case 6: send_trigger_state(AK_BUTTON_L2, 10, 930, 625, down, &l2_pressed); break;
    case 7: send_trigger_state(AK_BUTTON_R2, 11, 1210, 625, down, &r2_pressed); break;
    case 8: handle_controller_button(SDL_CONTROLLER_BUTTON_BACK, down); break;
    case 9: handle_controller_button(SDL_CONTROLLER_BUTTON_START, down); break;
    case 10: handle_controller_button(SDL_CONTROLLER_BUTTON_LEFTSTICK, down); break;
    case 11: handle_controller_button(SDL_CONTROLLER_BUTTON_RIGHTSTICK, down); break;
    case 12: handle_controller_button(SDL_CONTROLLER_BUTTON_DPAD_UP, down); break;
    case 13: handle_controller_button(SDL_CONTROLLER_BUTTON_DPAD_DOWN, down); break;
    case 14: handle_controller_button(SDL_CONTROLLER_BUTTON_DPAD_LEFT, down); break;
    case 15: handle_controller_button(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, down); break;
    case 16: handle_controller_button(SDL_CONTROLLER_BUTTON_GUIDE, down); break;
    default: break;
  }
}

static void handle_hat(int old_value, int new_value) {
  if ((old_value & SDL_HAT_UP) != (new_value & SDL_HAT_UP))
    handle_controller_button(SDL_CONTROLLER_BUTTON_DPAD_UP, (new_value & SDL_HAT_UP) != 0);
  if ((old_value & SDL_HAT_DOWN) != (new_value & SDL_HAT_DOWN))
    handle_controller_button(SDL_CONTROLLER_BUTTON_DPAD_DOWN, (new_value & SDL_HAT_DOWN) != 0);
  if ((old_value & SDL_HAT_LEFT) != (new_value & SDL_HAT_LEFT))
    handle_controller_button(SDL_CONTROLLER_BUTTON_DPAD_LEFT, (new_value & SDL_HAT_LEFT) != 0);
  if ((old_value & SDL_HAT_RIGHT) != (new_value & SDL_HAT_RIGHT))
    handle_controller_button(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, (new_value & SDL_HAT_RIGHT) != 0);
}

static void handle_keyboard_input(SDL_Keycode sym, int down) {
  switch (sym) {
    case SDLK_ESCAPE:
      send_key(AK_BACK, down);
      set_button_touch(7, down, 70, 55);
      break;
    case SDLK_RETURN:
    case SDLK_SPACE:
      send_key(AK_ENTER, down);
      send_key(AK_DPAD_CENTER, down);
      send_key(AK_BUTTON_START, down);
      menu_touch(12, down, 640, 360);
      set_button_touch(6, down, 640, 675);
      break;
    case SDLK_UP:
      send_key(AK_DPAD_UP, down);
      menu_touch(13, down, 640, 300);
      break;
    case SDLK_DOWN:
      send_key(AK_DPAD_DOWN, down);
      menu_touch(14, down, 640, 420);
      break;
    case SDLK_LEFT:
      send_key(AK_DPAD_LEFT, down);
      menu_touch(15, down, 500, 360);
      break;
    case SDLK_RIGHT:
      send_key(AK_DPAD_RIGHT, down);
      menu_touch(15, down, 780, 360);
      break;
    case SDLK_w:
      d_up = down;
      update_left_touch();
      break;
    case SDLK_s:
      d_down = down;
      update_left_touch();
      break;
    case SDLK_a:
      d_left = down;
      update_left_touch();
      break;
    case SDLK_d:
      d_right = down;
      update_left_touch();
      break;
    case SDLK_i:
      r_up = down;
      update_right_touch();
      break;
    case SDLK_k:
      r_down = down;
      update_right_touch();
      break;
    case SDLK_j:
      r_left = down;
      update_right_touch();
      break;
    case SDLK_l:
      r_right = down;
      update_right_touch();
      break;
    case SDLK_z:
      send_key(AK_BUTTON_A, down);
      send_key(AK_ENTER, down);
      send_key(AK_DPAD_CENTER, down);
      set_button_touch(1, down, 1110, 615);
      break;
    case SDLK_x:
      send_key(AK_BUTTON_B, down);
      set_button_touch(2, down, 1210, 515);
      break;
    case SDLK_c:
      send_key(AK_BUTTON_X, down);
      set_button_touch(3, down, 1010, 515);
      break;
    case SDLK_v:
      send_key(AK_BUTTON_Y, down);
      set_button_touch(4, down, 1110, 390);
      break;
    case SDLK_q:
      send_key(AK_BUTTON_L1, down);
      set_button_touch(8, down, 930, 625);
      break;
    case SDLK_e:
      send_key(AK_BUTTON_R1, down);
      set_button_touch(9, down, 1210, 625);
      break;
    case SDLK_f:
      send_key(AK_BUTTON_L2, down);
      set_button_touch(10, down, 930, 625);
      break;
    case SDLK_g:
      send_key(AK_BUTTON_R2, down);
      set_button_touch(11, down, 1210, 625);
      break;
    case SDLK_n:
      send_key(AK_BUTTON_THUMBL, down);
      break;
    case SDLK_m:
      send_key(AK_BUTTON_THUMBR, down);
      break;
    default:
      break;
  }
}

#define MAX_INPUT_DEVICES 8
static SDL_GameController *g_gamepads[MAX_INPUT_DEVICES];
static SDL_JoystickID g_gamepad_ids[MAX_INPUT_DEVICES];
static SDL_Joystick *g_joysticks[MAX_INPUT_DEVICES];
static SDL_JoystickID g_joystick_ids[MAX_INPUT_DEVICES];
static int g_joystick_hats[MAX_INPUT_DEVICES];

static int find_gamepad_slot(SDL_JoystickID id) {
  for (int i = 0; i < MAX_INPUT_DEVICES; i++)
    if (g_gamepads[i] && g_gamepad_ids[i] == id) return i;
  return -1;
}

static int find_joystick_slot(SDL_JoystickID id) {
  for (int i = 0; i < MAX_INPUT_DEVICES; i++)
    if (g_joysticks[i] && g_joystick_ids[i] == id) return i;
  return -1;
}

static int free_gamepad_slot(void) {
  for (int i = 0; i < MAX_INPUT_DEVICES; i++)
    if (!g_gamepads[i]) return i;
  return -1;
}

static int free_joystick_slot(void) {
  for (int i = 0; i < MAX_INPUT_DEVICES; i++)
    if (!g_joysticks[i]) return i;
  return -1;
}

static void close_joystick_slot(int slot) {
  if (slot < 0 || slot >= MAX_INPUT_DEVICES || !g_joysticks[slot]) return;
  fprintf(stderr, "joystick removed: id=%d\n", (int)g_joystick_ids[slot]);
  SDL_JoystickClose(g_joysticks[slot]);
  g_joysticks[slot] = NULL;
  g_joystick_ids[slot] = -1;
  g_joystick_hats[slot] = 0;
}

static void close_gamepad_slot(int slot) {
  if (slot < 0 || slot >= MAX_INPUT_DEVICES || !g_gamepads[slot]) return;
  fprintf(stderr, "controller removed: id=%d\n", (int)g_gamepad_ids[slot]);
  SDL_GameControllerClose(g_gamepads[slot]);
  g_gamepads[slot] = NULL;
  g_gamepad_ids[slot] = -1;
}

static void close_input_device(SDL_JoystickID id) {
  close_gamepad_slot(find_gamepad_slot(id));
  close_joystick_slot(find_joystick_slot(id));
}

static void load_controller_mappings(void) {
  const char *paths[] = {
    getenv("DEADSPACE_GAMECONTROLLERDB"),
    getenv("SDL_GAMECONTROLLERCONFIG_FILE"),
    "./gamecontrollerdb.txt",
    "/storage/roms/ports/PortMaster/gamecontrollerdb.txt",
    "/storage/roms/ports/PortMaster/batocera/gamecontrollerdb.txt",
    "/storage/roms/ports/PortMaster/knulli/gamecontrollerdb.txt",
    "/roms/ports/PortMaster/gamecontrollerdb.txt",
    NULL
  };
  for (int i = 0; paths[i]; i++) {
    if (!paths[i] || !*paths[i] || access(paths[i], R_OK) != 0) continue;
    int n = SDL_GameControllerAddMappingsFromFile(paths[i]);
    if (n >= 0) fprintf(stderr, "controllerdb: %s (+%d)\n", paths[i], n);
  }
}

static void open_input_device(int index) {
  if (index < 0) return;
  if (SDL_IsGameController(index)) {
    SDL_GameController *gc = SDL_GameControllerOpen(index);
    if (!gc) {
      fprintf(stderr, "controller open failed index=%d: %s\n", index, SDL_GetError());
      return;
    }
    SDL_Joystick *joy = SDL_GameControllerGetJoystick(gc);
    SDL_JoystickID id = joy ? SDL_JoystickInstanceID(joy) : -1;
    if (find_gamepad_slot(id) >= 0) {
      SDL_GameControllerClose(gc);
      return;
    }
    close_joystick_slot(find_joystick_slot(id));
    int slot = free_gamepad_slot();
    if (slot < 0) {
      fprintf(stderr, "controller ignored, no free slot: %s\n", SDL_GameControllerName(gc));
      SDL_GameControllerClose(gc);
      return;
    }
    g_gamepads[slot] = gc;
    g_gamepad_ids[slot] = id;
    fprintf(stderr, "controller: %s id=%d\n", SDL_GameControllerName(gc), (int)id);
    return;
  }

  SDL_Joystick *joy = SDL_JoystickOpen(index);
  if (!joy) {
    fprintf(stderr, "joystick open failed index=%d: %s\n", index, SDL_GetError());
    return;
  }
  SDL_JoystickID id = SDL_JoystickInstanceID(joy);
  if (find_joystick_slot(id) >= 0 || find_gamepad_slot(id) >= 0) {
    SDL_JoystickClose(joy);
    return;
  }
  int slot = free_joystick_slot();
  if (slot < 0) {
    fprintf(stderr, "joystick ignored, no free slot: %s\n", SDL_JoystickName(joy));
    SDL_JoystickClose(joy);
    return;
  }
  g_joysticks[slot] = joy;
  g_joystick_ids[slot] = id;
  g_joystick_hats[slot] = 0;
  fprintf(stderr, "joystick fallback: %s id=%d axes=%d buttons=%d hats=%d\n",
          SDL_JoystickName(joy), (int)id, SDL_JoystickNumAxes(joy),
          SDL_JoystickNumButtons(joy), SDL_JoystickNumHats(joy));
}

static void open_all_input_devices(void) {
  for (int i = 0; i < SDL_NumJoysticks(); i++) open_input_device(i);
}

static void handle_joy_axis(int axis, int value) {
  switch (axis) {
    case 0: lx = value; update_left_touch(); break;
    case 1: ly = value; update_left_touch(); break;
    case 2: rx = value; update_right_touch(); break;
    case 3: ry = value; update_right_touch(); break;
    case 4: update_trigger_axis(AK_BUTTON_L2, 10, 930, 625, value, &l2_pressed); break;
    case 5: update_trigger_axis(AK_BUTTON_R2, 11, 1210, 625, value, &r2_pressed); break;
    default: break;
  }
}

static int event_input_enabled(void) {
  return env_flag("DS_EVENT_INPUT");
}

static int input_abs(int v) {
  if (v == -32768) return 32768;
  return v < 0 ? -v : v;
}

static void pick_dominant_axis(int *dst, int value) {
  if (input_abs(value) > input_abs(*dst)) *dst = value;
}

static int joy_button_index_for(SDL_GameControllerButton b) {
  switch (b) {
    case SDL_CONTROLLER_BUTTON_A: return 0;
    case SDL_CONTROLLER_BUTTON_B: return 1;
    case SDL_CONTROLLER_BUTTON_X: return 2;
    case SDL_CONTROLLER_BUTTON_Y: return 3;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return 4;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return 5;
    case SDL_CONTROLLER_BUTTON_BACK: return 8;
    case SDL_CONTROLLER_BUTTON_START: return 9;
    case SDL_CONTROLLER_BUTTON_LEFTSTICK: return 10;
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return 11;
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return 12;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return 13;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return 14;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return 15;
    case SDL_CONTROLLER_BUTTON_GUIDE: return 16;
    default: return -1;
  }
}

static int joystick_button_any(int button) {
  if (button < 0) return 0;
  for (int i = 0; i < MAX_INPUT_DEVICES; i++) {
    SDL_Joystick *joy = g_joysticks[i];
    if (!joy || SDL_JoystickNumButtons(joy) <= button) continue;
    if (SDL_JoystickGetButton(joy, button)) return 1;
  }
  return 0;
}

static int joystick_hat_any(int mask) {
  for (int i = 0; i < MAX_INPUT_DEVICES; i++) {
    SDL_Joystick *joy = g_joysticks[i];
    if (!joy || SDL_JoystickNumHats(joy) <= 0) continue;
    if (SDL_JoystickGetHat(joy, 0) & mask) return 1;
  }
  return 0;
}

static int polled_button_down(SDL_GameControllerButton b) {
  for (int i = 0; i < MAX_INPUT_DEVICES; i++) {
    if (g_gamepads[i] && SDL_GameControllerGetButton(g_gamepads[i], b)) return 1;
  }

  int joy_button = joy_button_index_for(b);
  if (joystick_button_any(joy_button)) return 1;

  switch (b) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return joystick_hat_any(SDL_HAT_UP);
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return joystick_hat_any(SDL_HAT_DOWN);
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return joystick_hat_any(SDL_HAT_LEFT);
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return joystick_hat_any(SDL_HAT_RIGHT);
    default: return 0;
  }
}

static void poll_digital_controls(void) {
  static const SDL_GameControllerButton buttons[] = {
    SDL_CONTROLLER_BUTTON_A,
    SDL_CONTROLLER_BUTTON_B,
    SDL_CONTROLLER_BUTTON_X,
    SDL_CONTROLLER_BUTTON_Y,
    SDL_CONTROLLER_BUTTON_BACK,
    SDL_CONTROLLER_BUTTON_GUIDE,
    SDL_CONTROLLER_BUTTON_START,
    SDL_CONTROLLER_BUTTON_LEFTSTICK,
    SDL_CONTROLLER_BUTTON_RIGHTSTICK,
    SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
    SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
    SDL_CONTROLLER_BUTTON_DPAD_UP,
    SDL_CONTROLLER_BUTTON_DPAD_DOWN,
    SDL_CONTROLLER_BUTTON_DPAD_LEFT,
    SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
  };

  /* debounce: adaptadores PS2 baratos flickeram dpad; exige 2 polls
   * consecutivos no mesmo estado antes de aceitar a transicao */
  static uint8_t pending_state[SDL_CONTROLLER_BUTTON_MAX];
  static uint8_t pending_count[SDL_CONTROLLER_BUTTON_MAX];
  for (int i = 0; i < (int)(sizeof(buttons) / sizeof(buttons[0])); i++) {
    SDL_GameControllerButton b = buttons[i];
    uint8_t down = polled_button_down(b) ? 1 : 0;
    if (down == g_polled_buttons[b]) {
      pending_count[b] = 0;
      continue;
    }
    if (pending_state[b] != down) {
      pending_state[b] = down;
      pending_count[b] = 1;
      continue;
    }
    if (++pending_count[b] < 2) continue;
    pending_count[b] = 0;
    g_polled_buttons[b] = down;
    handle_controller_button(b, down);
  }

  if (g_polled_buttons[SDL_CONTROLLER_BUTTON_BACK] &&
      g_polled_buttons[SDL_CONTROLLER_BUTTON_START])
    g_running = 0;
}

static void poll_analog_controls(void) {
  int nlx = 0, nly = 0, nrx = 0, nry = 0, nl2 = 0, nr2 = 0;

  for (int i = 0; i < MAX_INPUT_DEVICES; i++) {
    SDL_GameController *gc = g_gamepads[i];
    if (!gc) continue;
    pick_dominant_axis(&nlx, SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX));
    pick_dominant_axis(&nly, SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY));
    pick_dominant_axis(&nrx, SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX));
    pick_dominant_axis(&nry, SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY));
    pick_dominant_axis(&nl2, SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT));
    pick_dominant_axis(&nr2, SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT));
  }

  for (int i = 0; i < MAX_INPUT_DEVICES; i++) {
    SDL_Joystick *joy = g_joysticks[i];
    if (!joy) continue;
    int axes = SDL_JoystickNumAxes(joy);
    if (axes > 0) pick_dominant_axis(&nlx, SDL_JoystickGetAxis(joy, 0));
    if (axes > 1) pick_dominant_axis(&nly, SDL_JoystickGetAxis(joy, 1));
    if (axes > 2) pick_dominant_axis(&nrx, SDL_JoystickGetAxis(joy, 2));
    if (axes > 3) pick_dominant_axis(&nry, SDL_JoystickGetAxis(joy, 3));
    if (axes > 4) pick_dominant_axis(&nl2, SDL_JoystickGetAxis(joy, 4));
    if (axes > 5) pick_dominant_axis(&nr2, SDL_JoystickGetAxis(joy, 5));
  }

  if (joystick_button_any(6)) nl2 = 32767;
  if (joystick_button_any(7)) nr2 = 32767;

  lx = nlx;
  ly = nly;
  rx = nrx;
  ry = nry;

  /* alimenta a rota nativa (hook na thread do jogo consome) */
  {
    float jx = axis_norm(nlx), jy = axis_norm(nly);
    float jm = sqrtf(jx * jx + jy * jy);
    if (jm < 0.14f) { jx = 0.0f; jy = 0.0f; }
    if (jx == 0.0f && jy == 0.0f) {
      /* dpad tambem anda no gameplay */
      jx = (g_polled_buttons[SDL_CONTROLLER_BUTTON_DPAD_RIGHT] ? 1.0f : 0.0f) -
           (g_polled_buttons[SDL_CONTROLLER_BUTTON_DPAD_LEFT] ? 1.0f : 0.0f);
      jy = (g_polled_buttons[SDL_CONTROLLER_BUTTON_DPAD_DOWN] ? 1.0f : 0.0f) -
           (g_polled_buttons[SDL_CONTROLLER_BUTTON_DPAD_UP] ? 1.0f : 0.0f);
    }
    if (jx == 0.0f && jy == 0.0f && (g_auto_lx != 0.0f || g_auto_ly != 0.0f)) {
      jx = g_auto_lx;
      jy = g_auto_ly;
    }
    g_njoy_lx = jx;
    g_njoy_ly = jy;
    float cx = axis_norm(nrx), cy = axis_norm(nry);
    float cm = sqrtf(cx * cx + cy * cy);
    if (cm < 0.14f) { cx = 0.0f; cy = 0.0f; }
    if (cx == 0.0f && cy == 0.0f && (g_auto_rx != 0.0f || g_auto_ry != 0.0f)) {
      cx = g_auto_rx;
      cy = g_auto_ry;
    }
    g_njoy_rx = cx;
    g_njoy_ry = cy;
  }

  if (native_gameplay_active()) {
    /* gameplay nativo: garante que nao ha touch virtual preso */
    if (g_touch[0].active) set_button_touch(0, 0, 360, 440);
    if (g_touch[5].active) release_right_touch();
    left_release_pending = 0;
    if (g_gp_cursor_mode) cursor_update_from_pad();
  } else if (menu_cursor_enabled()) {
    cursor_update_from_pad();
  } else {
    update_left_touch();
    update_right_touch();
  }
  update_trigger_axis(AK_BUTTON_L2, 10, 930, 625, nl2, &l2_pressed);
  update_trigger_axis(AK_BUTTON_R2, 11, 1210, 625, nr2, &r2_pressed);
}

static void poll_input_devices(void) {
  if (event_input_enabled()) return;
  SDL_GameControllerUpdate();
  SDL_JoystickUpdate();
  poll_digital_controls();
  poll_analog_controls();

  if (control_log_enabled()) {
    static int raw_tick;
    if ((++raw_tick % 240) == 0) {
      for (int i = 0; i < MAX_INPUT_DEVICES; i++) {
        SDL_GameController *gc = g_gamepads[i];
        if (!gc) continue;
        SDL_Joystick *joy = SDL_GameControllerGetJoystick(gc);
        fprintf(stderr, "[ctl] raw gc%d hat0=%d ax=[%d %d %d %d] dpad=%d%d%d%d\n", i,
                joy && SDL_JoystickNumHats(joy) > 0 ? SDL_JoystickGetHat(joy, 0) : -1,
                SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX),
                SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY),
                SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX),
                SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY),
                SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_UP),
                SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN),
                SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT),
                SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT));
      }
    }
  }
}

static void gl_probe_frame(int frame) {
  if (!env_flag("DS_GLPROBE")) return;

  GLint viewport[4] = {0, 0, 0, 0};
  GLint scissor_box[4] = {0, 0, 0, 0};
  GLint gl_fbo = 0;
  GLint tex_binding = 0;
  GLboolean color_mask[4] = {0, 0, 0, 0};
  GLfloat current_color[4] = {0, 0, 0, 0};
  GLboolean en_tex2d = 0, en_blend = 0, en_alpha = 0, en_depth = 0;
  GLboolean en_cull = 0, en_scissor = 0;
  GLboolean en_vertex_array = 0, en_color_array = 0, en_texcoord_array = 0;
  unsigned char px[4 * 9];
  unsigned long sum = 0;
  unsigned char minv = 255, maxv = 0;
  unsigned draw_calls = 0, default_draws = 0, fbo_draws = 0, tracked_fbo = 0;
  unsigned err_before = glGetError();

  deadspace_gl_take_draw_stats(&draw_calls, &default_draws, &fbo_draws, &tracked_fbo);
  glGetIntegerv(GL_VIEWPORT, viewport);
  glGetIntegerv(GL_SCISSOR_BOX, scissor_box);
  glGetIntegerv(GL_FRAMEBUFFER_BINDING_OES, &gl_fbo);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex_binding);
  glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
  glGetFloatv(GL_CURRENT_COLOR, current_color);
  en_tex2d = glIsEnabled(GL_TEXTURE_2D);
  en_blend = glIsEnabled(GL_BLEND);
  en_alpha = glIsEnabled(GL_ALPHA_TEST);
  en_depth = glIsEnabled(GL_DEPTH_TEST);
  en_cull = glIsEnabled(GL_CULL_FACE);
  en_scissor = glIsEnabled(GL_SCISSOR_TEST);
  en_vertex_array = glIsEnabled(GL_VERTEX_ARRAY);
  en_color_array = glIsEnabled(GL_COLOR_ARRAY);
  en_texcoord_array = glIsEnabled(GL_TEXTURE_COORD_ARRAY);
  int xs[3] = {g_w / 4, g_w / 2, (g_w * 3) / 4};
  int ys[3] = {g_h / 4, g_h / 2, (g_h * 3) / 4};
  int k = 0;
  for (int y = 0; y < 3; y++) {
    for (int x = 0; x < 3; x++) {
      memset(&px[k * 4], 0, 4);
      glReadPixels(xs[x], ys[y], 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &px[k * 4]);
      for (int c = 0; c < 4; c++) {
        unsigned char v = px[k * 4 + c];
        sum += v;
        if (v < minv) minv = v;
        if (v > maxv) maxv = v;
      }
      k++;
    }
  }
  unsigned err_after = glGetError();
  fprintf(stderr,
          "[glprobe] frame=%d viewport=%d,%d %dx%d err_before=0x%x err_after=0x%x "
          "draws=%u default=%u fbo=%u bind=%d tracked=%u "
          "mask=%u%u%u%u color=%.2f,%.2f,%.2f,%.2f tex=%d "
          "en=T%d/B%d/A%d/D%d/C%d/S%d arr=V%d/C%d/T%d scissor=%d,%d %dx%d "
          "samples_min=%u samples_max=%u samples_sum=%lu center=%u,%u,%u,%u\n",
          frame, viewport[0], viewport[1], viewport[2], viewport[3], err_before, err_after,
          draw_calls, default_draws, fbo_draws, gl_fbo, tracked_fbo,
          color_mask[0], color_mask[1], color_mask[2], color_mask[3],
          current_color[0], current_color[1], current_color[2], current_color[3],
          tex_binding,
          en_tex2d, en_blend, en_alpha, en_depth, en_cull, en_scissor,
          en_vertex_array, en_color_array, en_texcoord_array,
          scissor_box[0], scissor_box[1], scissor_box[2], scissor_box[3],
          minv, maxv, sum, px[4 * 4 + 0], px[4 * 4 + 1], px[4 * 4 + 2], px[4 * 4 + 3]);
}

static void gl_dump_frame(int frame) {
  int dump_frame = env_int("DS_GLDUMP_FRAME", -1);
  int dump_every = env_int("DS_GLDUMP_EVERY", 0);
  static int dumped;
  if (dump_every > 0) {
    if (frame == 0 || (frame % dump_every) != 0) return;
    if (frame / dump_every > env_int("DS_GLDUMP_MAX", 40)) return;
  } else {
    if (dumped || dump_frame < 0 || frame != dump_frame) return;
  }

  size_t pixels_sz = (size_t)g_w * (size_t)g_h * 4u;
  unsigned char *pixels = malloc(pixels_sz);
  if (!pixels) return;

  glReadPixels(0, 0, g_w, g_h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
  char path[256];
  const char *custom = getenv("DS_GLDUMP_PATH");
  if (custom && *custom) snprintf(path, sizeof(path), "%s", custom);
  else snprintf(path, sizeof(path), "/tmp/deadspace-frame-%d.ppm", frame);

  FILE *fp = fopen(path, "wb");
  if (fp) {
    fprintf(fp, "P6\n%d %d\n255\n", g_w, g_h);
    for (int y = g_h - 1; y >= 0; y--) {
      unsigned char *row = pixels + (size_t)y * (size_t)g_w * 4u;
      for (int x = 0; x < g_w; x++) {
        fwrite(row + x * 4, 1, 3, fp);
      }
    }
    fclose(fp);
    fprintf(stderr, "[gldump] wrote %s\n", path);
  }
  free(pixels);
  dumped = 1;
}

static void auto_test_input(int frame) {
  if (!env_flag("DS_AUTOINPUT")) return;
  /* fase de menus: tap central (tap-to-continue), Play (165,650) e
   * confirmacao de dificuldade; para quando o gameplay nativo assume */
  int accept_phase = frame >= 30 && frame <= 3600 && !native_gameplay_active();
  if (accept_phase && (frame % 240) == 30) {
    send_key(AK_ENTER, 1);
    send_key(AK_DPAD_CENTER, 1);
    touch_down(0, sx(640), sy(360));
  }
  if (accept_phase && (frame % 240) == 38) {
    send_key(AK_ENTER, 0);
    send_key(AK_DPAD_CENTER, 0);
    touch_up(0, sx(640), sy(360));
  }
  if (accept_phase && (frame % 240) == 100) {
    touch_down(0, sx(165), sy(650));  /* Play no menu principal */
  }
  if (accept_phase && (frame % 240) == 108) {
    touch_up(0, sx(165), sy(650));
  }
  if (accept_phase && (frame % 240) == 170) {
    touch_down(0, sx(env_int("DS_AUTO_DIFF_X", 640)), sy(env_int("DS_AUTO_DIFF_Y", 300)));
  }
  if (accept_phase && (frame % 240) == 178) {
    touch_up(0, sx(env_int("DS_AUTO_DIFF_X", 640)), sy(env_int("DS_AUTO_DIFF_Y", 300)));
  }
  /* fase de gameplay: relogio proprio a partir do momento em que a rota
   * nativa ficou ativa (hook vivo + hud despausado) */
  static int gp_frames = -1;
  if (native_gameplay_active()) {
    if (gp_frames < 0) {
      gp_frames = 0;
      fprintf(stderr, "[auto] gameplay nativo detectado no frame %d\n", frame);
    }
    gp_frames++;
    switch (gp_frames) {
      case 60:  g_auto_ly = -1.0f; fprintf(stderr, "[auto] andar frente\n"); break;
      case 300: g_auto_ly = 0.0f; fprintf(stderr, "[auto] parar\n"); break;
      case 360: g_auto_rx = 1.0f; fprintf(stderr, "[auto] camera direita\n"); break;
      case 600: g_auto_rx = 0.0f; fprintf(stderr, "[auto] camera parar\n"); break;
      case 660: native_button(NBTN_LT, 1); fprintf(stderr, "[auto] mira on\n"); break;
      case 780: native_button(NBTN_RT, 1); fprintf(stderr, "[auto] tiro\n"); break;
      case 900: native_button(NBTN_LT, 0); fprintf(stderr, "[auto] mira off\n"); break;
      case 960: native_button(NBTN_X, 1); fprintf(stderr, "[auto] reload\n"); break;
      case 1080: native_button(NBTN_B, 1); fprintf(stderr, "[auto] melee\n"); break;
      default: break;
    }
  }
}

static void *entry_thread(void *arg) {
  (void)arg;
  if (p_runEntryPoint) {
    fprintf(stderr, "runEntryPoint thread start\n");
    int r = p_runEntryPoint(g_env, NULL);
    fprintf(stderr, "runEntryPoint returned %d\n", r);
  }
  return NULL;
}

static int init_sdl(SDL_Window **out_win, SDL_GLContext *out_gl) {
  SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0) {
    fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return -1;
  }
  fprintf(stderr, "SDL audio driver current=%s\n",
          SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "(none)");
  SDL_GameControllerEventState(SDL_ENABLE);
  SDL_JoystickEventState(SDL_ENABLE);
  load_controller_mappings();
  SDL_DisplayMode dm;
  if (SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.w > 0 && dm.h > 0) {
    g_w = dm.w;
    g_h = dm.h;
  }
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 1);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
  SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_Window *win = SDL_CreateWindow("Dead Space", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                     g_w, g_h, SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN);
  if (!win) {
    fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
    return -1;
  }
  SDL_GLContext gl = SDL_GL_CreateContext(win);
  if (!gl) {
    fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
    return -1;
  }
  SDL_GL_SetSwapInterval(1);
  SDL_GL_GetDrawableSize(win, &g_w, &g_h);
  fprintf(stderr, "SDL GLES1 window %dx%d\n", g_w, g_h);

  g_audio_ring = calloc(AUDIO_RING_SAMPLES, sizeof(int16_t));
  SDL_AudioSpec want, have;
  memset(&want, 0, sizeof(want));
  want.freq = 44100;
  want.format = AUDIO_S16LSB;
  want.channels = 2;
  want.samples = 1024;
  want.callback = audio_cb;
  g_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
  if (g_audio_dev) {
    g_audio_paused = 0;
    SDL_PauseAudioDevice(g_audio_dev, 0);
    fprintf(stderr, "SDL audio %d Hz %d ch fmt=0x%x samples=%d driver=%s dev=%u\n",
            have.freq, have.channels, have.format, have.samples,
            SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "(none)",
            (unsigned)g_audio_dev);
  } else {
    fprintf(stderr, "SDL audio disabled: %s\n", SDL_GetError());
  }

  open_all_input_devices();

  *out_win = win;
  *out_gl = gl;
  return 0;
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  install_crash_handler();
  setenv("DEADSPACE_HOME", ".", 0);
  setenv("DEADSPACE_TMP", "/tmp", 0);

  SDL_Window *win = NULL;
  SDL_GLContext gl = NULL;
  if (init_sdl(&win, &gl) != 0) return 1;
  jni_set_display_size(g_w, g_h);
  jni_set_asset_root(getenv("DEADSPACE_ASSETS") ? getenv("DEADSPACE_ASSETS") : "assets");
  jni_set_audio_output(audio_write, audio_state);

  preload_device_libs();

  int import_n = 0;
  DynLibFunction *imports = build_import_table(&import_n);
  size_t heap_size = (size_t)GAME_HEAP_MB * 1024 * 1024;
  void *heap = mmap(NULL, heap_size, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (heap == MAP_FAILED) {
    perror("mmap");
    return 1;
  }
  if (so_load(SO_NAME, heap, heap_size) < 0) {
    fprintf(stderr, "so_load %s failed\n", SO_NAME);
    return 1;
  }
  g_load_base = (uintptr_t)text_base;
  fprintf(stderr, "Loaded %s base=0x%lx text=%p+%zu data=%p+%zu\n",
          SO_NAME, (unsigned long)g_load_base, text_base, text_size, data_base, data_size);
  if (so_relocate() < 0) return 1;
  so_resolve(imports, import_n, 0);
  patch_got_slot("raise", (uintptr_t)deadspace_raise_stub());
  patch_got_slot("abort", (uintptr_t)deadspace_abort_stub());
  patch_got_slot("setjmp", (uintptr_t)_setjmp);
  patch_got_slot("longjmp", (uintptr_t)_longjmp);
  install_core_allocator_patch();
  install_fontfusion_allocator_patch();
  install_vfs_open_hook();
  install_filestream_published_patch();
  install_texturepack_empty_guard();
  install_cinematic_model_null_guard();
  install_hemisphere_map_null_guard();
  install_animplayer_setnode_null_guard();
  install_playable_rig_model_null_guard();
  install_animplayer_offset_null_guard();
  install_animplayer_setanim_null_guard();
  install_plasmacutter_model_null_guard();
  install_ripper_model_null_guard();
  install_accelerometer_null_guard();
  install_accelerometer_device_null_guards();
  native_btn_load_env();
  install_native_input_hook();
  so_finalize();
  so_flush_caches();
  so_execute_init_array();

  jni_shim_init(&g_vm, &g_env);
  find_exports();
  if (p_JNI_OnLoad) {
    jint ver = p_JNI_OnLoad(g_vm, NULL);
    fprintf(stderr, "JNI_OnLoad -> 0x%x\n", ver);
  }

  void *asset_mgr = jni_asset_manager();
  if (p_EAIO_Startup) p_EAIO_Startup(g_env, NULL, asset_mgr);
  if (p_rwfs_Startup) p_rwfs_Startup(g_env, NULL, asset_mgr);

  if (p_Audio_Init) p_Audio_Init(g_env, NULL, jni_audio_track_object(), 2048, 2, 44100);

  if (p_ModPhysicalKeyboard) g_mod_key = p_ModPhysicalKeyboard(g_env, NULL);
  if (p_ModTouchScreen) g_mod_touch = p_ModTouchScreen(g_env, NULL);
  if (p_ModTouchPad) g_mod_touchpad = p_ModTouchPad(g_env, NULL);
  if (p_RawDown) g_raw_down = p_RawDown(g_env, NULL);
  if (p_RawMove) g_raw_move = p_RawMove(g_env, NULL);
  if (p_RawUp) g_raw_up = p_RawUp(g_env, NULL);
  if (p_RawCancel) g_raw_cancel = p_RawCancel(g_env, NULL);
  fprintf(stderr, "modules key=%d touch=%d touchpad=%d raw down/move/up/cancel=%d/%d/%d/%d\n",
          g_mod_key, g_mod_touch, g_mod_touchpad,
          g_raw_down, g_raw_move, g_raw_up, g_raw_cancel);

  if (p_NativeOnCreate) {
    fprintf(stderr, "NativeOnCreate\n");
    p_NativeOnCreate(g_env, jni_activity_object());
  }

  pthread_t entry;
  if (p_runEntryPoint && !getenv("DS_NORUNENTRY")) pthread_create(&entry, NULL, entry_thread, NULL);

  if (p_SurfaceCreated) {
    fprintf(stderr, "SurfaceCreated\n");
    p_SurfaceCreated(g_env, jni_renderer_object());
  }
  if (p_SurfaceChanged) {
    fprintf(stderr, "SurfaceChanged %dx%d\n", g_w, g_h);
    p_SurfaceChanged(g_env, jni_renderer_object(), g_w, g_h);
  }
  if (p_NativeOnResume) {
    fprintf(stderr, "NativeOnResume\n");
    p_NativeOnResume(g_env, jni_activity_object());
  }
  if (p_NativeOnWindowFocusChanged) {
    fprintf(stderr, "WindowFocus true\n");
    p_NativeOnWindowFocusChanged(g_env, jni_activity_object(), 1);
  }

  install_vfs_asset_mounts();

  fprintf(stderr, "Entering render loop\n");
  uint32_t last = SDL_GetTicks();
  while (g_running) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) g_running = 0;
      else if (env_flag("DS_IGNORE_SDL_INPUT")) {
        continue;
      }
      else if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
        int down = e.type == SDL_KEYDOWN;
        handle_keyboard_input(e.key.keysym.sym, down);
      } else if (e.type == SDL_CONTROLLERDEVICEADDED) {
        open_input_device(e.cdevice.which);
      } else if (e.type == SDL_CONTROLLERDEVICEREMOVED) {
        close_input_device(e.cdevice.which);
      } else if (e.type == SDL_JOYDEVICEADDED) {
        open_input_device(e.jdevice.which);
      } else if (e.type == SDL_JOYDEVICEREMOVED) {
        close_input_device(e.jdevice.which);
      } else if (!event_input_enabled() &&
                 (e.type == SDL_CONTROLLERBUTTONDOWN || e.type == SDL_CONTROLLERBUTTONUP ||
                  e.type == SDL_CONTROLLERAXISMOTION ||
                  e.type == SDL_JOYBUTTONDOWN || e.type == SDL_JOYBUTTONUP ||
                  e.type == SDL_JOYAXISMOTION || e.type == SDL_JOYHATMOTION)) {
        continue;
      } else if (e.type == SDL_CONTROLLERBUTTONDOWN || e.type == SDL_CONTROLLERBUTTONUP) {
        int down = e.type == SDL_CONTROLLERBUTTONDOWN;
        SDL_GameControllerButton b = (SDL_GameControllerButton)e.cbutton.button;
        SDL_GameController *gc = SDL_GameControllerFromInstanceID(e.cbutton.which);
        handle_controller_button(b, down);
        if (gc && down && b == SDL_CONTROLLER_BUTTON_BACK && SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_START))
          g_running = 0;
        if (gc && down && b == SDL_CONTROLLER_BUTTON_START && SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_BACK))
          g_running = 0;
      } else if (e.type == SDL_CONTROLLERAXISMOTION) {
        int v = e.caxis.value;
        switch (e.caxis.axis) {
          case SDL_CONTROLLER_AXIS_LEFTX: lx = v; update_left_touch(); break;
          case SDL_CONTROLLER_AXIS_LEFTY: ly = v; update_left_touch(); break;
          case SDL_CONTROLLER_AXIS_RIGHTX: rx = v; update_right_touch(); break;
          case SDL_CONTROLLER_AXIS_RIGHTY: ry = v; update_right_touch(); break;
          case SDL_CONTROLLER_AXIS_TRIGGERLEFT: update_trigger_axis(AK_BUTTON_L2, 10, 930, 625, v, &l2_pressed); break;
          case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: update_trigger_axis(AK_BUTTON_R2, 11, 1210, 625, v, &r2_pressed); break;
          default: break;
        }
      } else if (e.type == SDL_JOYBUTTONDOWN || e.type == SDL_JOYBUTTONUP) {
        if (find_gamepad_slot(e.jbutton.which) < 0)
          handle_joy_button(e.jbutton.button, e.type == SDL_JOYBUTTONDOWN);
      } else if (e.type == SDL_JOYAXISMOTION) {
        if (find_gamepad_slot(e.jaxis.which) < 0)
          handle_joy_axis(e.jaxis.axis, e.jaxis.value);
      } else if (e.type == SDL_JOYHATMOTION) {
        if (find_gamepad_slot(e.jhat.which) < 0) {
          int slot = find_joystick_slot(e.jhat.which);
          int old_hat = slot >= 0 ? g_joystick_hats[slot] : 0;
          handle_hat(old_hat, e.jhat.value);
          if (slot >= 0) g_joystick_hats[slot] = e.jhat.value;
        }
      }
    }

    static int frame_count = 0;
    if (!env_flag("DS_IGNORE_SDL_INPUT")) poll_input_devices();
    auto_test_input(frame_count);
    expire_drag_releases();
    if ((frame_count & 1) == 0) {
      refresh_touch_slot(0);
      refresh_touch_slot(5);
    }
    if (p_DrawFrame && !getenv("DS_NODRAW")) {
      int frame_log = frame_count;
      if (frame_log < 8) fprintf(stderr, "DrawFrame %d\n", frame_log);
      if (env_flag("DS_TEST_CLEAR")) {
        glClearColor(0.0f, 0.25f, 0.75f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
      }
      p_DrawFrame(g_env, jni_renderer_object());
      draw_menu_cursor();
      if (frame_log < 8 || (env_flag("DS_GLPROBE") && (frame_log % 120) == 0))
        gl_probe_frame(frame_log);
      gl_dump_frame(frame_log);
    }
    frame_count++;
    SDL_GL_SwapWindow(win);
    uint32_t now = SDL_GetTicks();
    uint32_t elapsed = now - last;
    if (elapsed < 16) SDL_Delay(16 - elapsed);
    last = SDL_GetTicks();
  }

  /* saida blindada: se qualquer shutdown do jogo travar, o SIGALRM mata
   * o processo e o launcher (trap EXIT) religa o EmulationStation */
  fprintf(stderr, "saindo (Select+Start)\n");
  fflush(NULL);
  alarm(4);
  if (p_NativeOnPause) p_NativeOnPause(g_env, jni_activity_object());
  audio_state(0);
  SDL_Quit();
  fflush(NULL);
  _exit(0);
}
