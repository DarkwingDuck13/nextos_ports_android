/* main.c -- Bully2 clean Android so-loader for NextOS Mali-450 first. */
#define _GNU_SOURCE
#include <ctype.h>
#include <dlfcn.h>
#include <signal.h>
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#include "so_util.h"

#define CXX_SO "libc++_shared.so"
#define GAME_SO "libGame.so"
#define CXX_HEAP_MB 48
#define GAME_HEAP_MB 160

int mod_game, mod_cxx;

__attribute__((used, aligned(16))) _Thread_local char g_bionic_guard_pad[256];

extern DynLibFunction bully_stub_table[];
extern const int bully_stub_count;
extern DynLibFunction revc_pthread_table[];
extern const int revc_pthread_count;
extern void bully_imports_init(void);
extern void jni_load(void);
extern void bully_gltrace_dump(FILE *out);

static DynLibFunction *g_base;
static int g_base_n;

static void print_map_for_addr(const char *label, uintptr_t addr) {
  FILE *f = fopen("/proc/self/maps", "r");
  if (!f)
    return;
  char line[512];
  while (fgets(line, sizeof(line), f)) {
    unsigned long start = 0, end = 0;
    if (sscanf(line, "%lx-%lx", &start, &end) == 2 &&
        addr >= (uintptr_t)start && addr < (uintptr_t)end) {
      fprintf(stderr, "  %s map: %s", label, line);
      break;
    }
  }
  fclose(f);
}

static void print_aarch64_frames(uintptr_t fp, uintptr_t sp) {
#if defined(__aarch64__)
  uintptr_t tb = (uintptr_t)text_base;
  fprintf(stderr, "  FP chain:\n");
  for (int i = 0; i < 24 && fp; i++) {
    if (fp < sp || (fp & 15))
      break;
    uintptr_t *frame = (uintptr_t *)fp;
    uintptr_t next = frame[0];
    uintptr_t ret = frame[1];
    fprintf(stderr, "    #%02d fp=%p ret=%p", i, (void *)fp, (void *)ret);
    if (tb && ret >= tb && ret < tb + text_size)
      fprintf(stderr, " = libGame+0x%lx", (unsigned long)(ret - tb));
    fprintf(stderr, "\n");
    if (next <= fp || next - fp > 1024 * 1024)
      break;
    fp = next;
  }
#else
  (void)fp;
  (void)sp;
#endif
}

static void build_base_table(void) {
  g_base_n = bully_stub_count + revc_pthread_count;
  g_base = malloc(sizeof(DynLibFunction) * (size_t)g_base_n);
  if (!g_base) {
    fprintf(stderr, "base table malloc failed\n");
    exit(1);
  }
  memcpy(g_base, bully_stub_table, sizeof(DynLibFunction) * bully_stub_count);
  memcpy(g_base + bully_stub_count, revc_pthread_table,
         sizeof(DynLibFunction) * revc_pthread_count);
}

static void crash_handler(int sig, siginfo_t *info, void *uc) {
  uintptr_t fault = (uintptr_t)info->si_addr;
  uintptr_t tb = (uintptr_t)text_base;
  fprintf(stderr, "\n=== CRASH sig=%d addr=%p ===\n", sig, info->si_addr);
  if (tb && fault >= tb && fault < tb + text_size)
    fprintf(stderr, "  fault libGame+0x%lx\n", (unsigned long)(fault - tb));
#if defined(__aarch64__)
  ucontext_t *u = (ucontext_t *)uc;
  uintptr_t pc = u->uc_mcontext.pc;
  uintptr_t lr = u->uc_mcontext.regs[30];
  fprintf(stderr, "  PC=%p", (void *)pc);
  if (tb && pc >= tb && pc < tb + text_size)
    fprintf(stderr, " = libGame+0x%lx", (unsigned long)(pc - tb));
  fprintf(stderr, "\n");
  fprintf(stderr, "  LR=%p", (void *)lr);
  if (tb && lr >= tb && lr < tb + text_size)
    fprintf(stderr, " = libGame+0x%lx", (unsigned long)(lr - tb));
  fprintf(stderr, " SP=%p x0=%p x1=%p x2=%p x3=%p\n",
          (void *)u->uc_mcontext.sp, (void *)u->uc_mcontext.regs[0],
          (void *)u->uc_mcontext.regs[1], (void *)u->uc_mcontext.regs[2],
          (void *)u->uc_mcontext.regs[3]);
  print_map_for_addr("PC", pc);
  print_map_for_addr("LR", lr);
  print_aarch64_frames(u->uc_mcontext.regs[29], u->uc_mcontext.sp);
#else
  (void)uc;
#endif
  bully_gltrace_dump(stderr);
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
}

static void preload_device_libs(void) {
  static const char *libs[] = {
      "libSDL2-2.0.so.0", "libGLESv2.so", "libEGL.so",
      "libopenal.so.1",   "libmpg123.so.0", "libm.so.6", NULL,
  };
  for (int i = 0; libs[i]; i++) {
    void *h = dlopen(libs[i], RTLD_NOW | RTLD_GLOBAL);
    fprintf(stderr, "preload: %s %s\n", libs[i], h ? "OK" : dlerror());
  }
}

static int env_mb(const char *name, int def, int min, int max) {
  const char *e = getenv(name);
  int v = e && *e ? atoi(e) : def;
  if (v < min)
    v = min;
  if (v > max)
    v = max;
  return v;
}

static const char *first_env(const char *a, const char *b) {
  const char *e = getenv(a);
  if (e && *e)
    return e;
  e = getenv(b);
  return (e && *e) ? e : NULL;
}

static int read_first_token(const char *path, char *buf, size_t len) {
  if (!path || !*path || !len)
    return 0;
  FILE *f = fopen(path, "rb");
  if (!f)
    return 0;
  size_t n = fread(buf, 1, len - 1, f);
  fclose(f);
  buf[n] = 0;
  for (size_t i = 0; i < n; i++) {
    if (buf[i] == '\r' || buf[i] == '\n' || buf[i] == '\t')
      buf[i] = ' ';
  }
  while (*buf == ' ')
    memmove(buf, buf + 1, strlen(buf));
  return buf[0] != 0;
}

static int texture_profile_is_high(const char *e) {
  if (!e || !*e)
    return 0;
  while (*e == ' ' || *e == '\t' || *e == '\n' || *e == '\r')
    e++;
  return !strncasecmp(e, "high", 4) || !strncasecmp(e, "full", 4) ||
         !strncasecmp(e, "native", 6) || !strncasecmp(e, "off", 3) ||
         !strcmp(e, "0");
}

static int default_game_heap_mb(void) {
  char file_profile[32];
  const char *profile = first_env("BULLY2_TEXTURE_PROFILE",
                                  "BULLY_TEXTURE_PROFILE");
  if (!profile)
    profile = first_env("BULLY2_TEX_HALF_MODE", "BULLY_TEX_HALF_MODE");
  if (!profile) {
    const char *path = first_env("BULLY2_TEX_PROFILE_SAVE",
                                 "BULLY_TEX_PROFILE_SAVE");
    if (!path || !*path)
      path = "texture_profile.cfg";
    if (read_first_token(path, file_profile, sizeof(file_profile)))
      profile = file_profile;
  }
  return texture_profile_is_high(profile) ? 160 : 128;
}

static int file_exists(const char *path) {
  struct stat st;
  return path && stat(path, &st) == 0;
}

static const char **glyph5x7(int c) {
  static const char *sp[] = {"00000", "00000", "00000", "00000", "00000", "00000", "00000"};
  static const char *q[] = {"01110", "10001", "00001", "00010", "00100", "00000", "00100"};
  static const char *dot[] = {"00000", "00000", "00000", "00000", "00000", "01100", "01100"};
  static const char *dash[] = {"00000", "00000", "00000", "11110", "00000", "00000", "00000"};
  static const char *slash[] = {"00001", "00010", "00010", "00100", "01000", "01000", "10000"};
  static const char *colon[] = {"00000", "01100", "01100", "00000", "01100", "01100", "00000"};
  static const char *pct[] = {"11001", "11010", "00100", "01000", "10110", "00110", "00000"};
  static const char *n0[] = {"01110", "10001", "10011", "10101", "11001", "10001", "01110"};
  static const char *n1[] = {"00100", "01100", "00100", "00100", "00100", "00100", "01110"};
  static const char *n2[] = {"01110", "10001", "00001", "00010", "00100", "01000", "11111"};
  static const char *n3[] = {"11110", "00001", "00001", "01110", "00001", "00001", "11110"};
  static const char *n4[] = {"00010", "00110", "01010", "10010", "11111", "00010", "00010"};
  static const char *n5[] = {"11111", "10000", "10000", "11110", "00001", "00001", "11110"};
  static const char *n6[] = {"00110", "01000", "10000", "11110", "10001", "10001", "01110"};
  static const char *n7[] = {"11111", "00001", "00010", "00100", "01000", "01000", "01000"};
  static const char *n8[] = {"01110", "10001", "10001", "01110", "10001", "10001", "01110"};
  static const char *n9[] = {"01110", "10001", "10001", "01111", "00001", "00010", "01100"};
  static const char *A[] = {"01110", "10001", "10001", "11111", "10001", "10001", "10001"};
  static const char *B[] = {"11110", "10001", "10001", "11110", "10001", "10001", "11110"};
  static const char *C[] = {"01110", "10001", "10000", "10000", "10000", "10001", "01110"};
  static const char *D[] = {"11110", "10001", "10001", "10001", "10001", "10001", "11110"};
  static const char *E[] = {"11111", "10000", "10000", "11110", "10000", "10000", "11111"};
  static const char *F[] = {"11111", "10000", "10000", "11110", "10000", "10000", "10000"};
  static const char *G[] = {"01110", "10001", "10000", "10111", "10001", "10001", "01110"};
  static const char *H[] = {"10001", "10001", "10001", "11111", "10001", "10001", "10001"};
  static const char *I[] = {"01110", "00100", "00100", "00100", "00100", "00100", "01110"};
  static const char *J[] = {"00001", "00001", "00001", "00001", "10001", "10001", "01110"};
  static const char *K[] = {"10001", "10010", "10100", "11000", "10100", "10010", "10001"};
  static const char *L[] = {"10000", "10000", "10000", "10000", "10000", "10000", "11111"};
  static const char *M[] = {"10001", "11011", "10101", "10101", "10001", "10001", "10001"};
  static const char *N[] = {"10001", "11001", "10101", "10011", "10001", "10001", "10001"};
  static const char *O[] = {"01110", "10001", "10001", "10001", "10001", "10001", "01110"};
  static const char *P[] = {"11110", "10001", "10001", "11110", "10000", "10000", "10000"};
  static const char *Q[] = {"01110", "10001", "10001", "10001", "10101", "10010", "01101"};
  static const char *R[] = {"11110", "10001", "10001", "11110", "10100", "10010", "10001"};
  static const char *S[] = {"01111", "10000", "10000", "01110", "00001", "00001", "11110"};
  static const char *T[] = {"11111", "00100", "00100", "00100", "00100", "00100", "00100"};
  static const char *U[] = {"10001", "10001", "10001", "10001", "10001", "10001", "01110"};
  static const char *V[] = {"10001", "10001", "10001", "10001", "10001", "01010", "00100"};
  static const char *W[] = {"10001", "10001", "10001", "10101", "10101", "10101", "01010"};
  static const char *X[] = {"10001", "10001", "01010", "00100", "01010", "10001", "10001"};
  static const char *Y[] = {"10001", "10001", "01010", "00100", "00100", "00100", "00100"};
  static const char *Z[] = {"11111", "00001", "00010", "00100", "01000", "10000", "11111"};
  if (c >= 'a' && c <= 'z')
    c = toupper(c);
  switch (c) {
  case '0': return n0; case '1': return n1; case '2': return n2; case '3': return n3; case '4': return n4;
  case '5': return n5; case '6': return n6; case '7': return n7; case '8': return n8; case '9': return n9;
  case 'A': return A; case 'B': return B; case 'C': return C; case 'D': return D; case 'E': return E;
  case 'F': return F; case 'G': return G; case 'H': return H; case 'I': return I; case 'J': return J;
  case 'K': return K; case 'L': return L; case 'M': return M; case 'N': return N; case 'O': return O;
  case 'P': return P; case 'Q': return Q; case 'R': return R; case 'S': return S; case 'T': return T;
  case 'U': return U; case 'V': return V; case 'W': return W; case 'X': return X; case 'Y': return Y; case 'Z': return Z;
  case '.': return dot; case '-': return dash; case '/': return slash; case ':': return colon; case '%': return pct;
  case ' ': return sp; default: return q;
  }
}

static void draw_text(SDL_Renderer *r, int x, int y, int scale, const char *s) {
  SDL_Rect px = {0, 0, scale, scale};
  for (const unsigned char *p = (const unsigned char *)s; p && *p; p++) {
    const char **g = glyph5x7(*p);
    for (int yy = 0; yy < 7; yy++) {
      for (int xx = 0; xx < 5; xx++) {
        if (g[yy][xx] == '1') {
          px.x = x + xx * scale;
          px.y = y + yy * scale;
          SDL_RenderFillRect(r, &px);
        }
      }
    }
    x += 6 * scale;
  }
}

static int read_setup_state(const char *path, int *state, int *done, int *total,
                            char *msg, size_t msg_sz) {
  FILE *f = fopen(path, "r");
  if (!f)
    return 0;
  char line[256];
  if (fgets(line, sizeof(line), f))
    sscanf(line, "%d %d %d", state, done, total);
  if (msg && msg_sz) {
    if (fgets(msg, (int)msg_sz, f)) {
      msg[msg_sz - 1] = 0;
      msg[strcspn(msg, "\r\n")] = 0;
    }
  }
  fclose(f);
  return 1;
}

static int run_setup_splash(void) {
  const char *setup = getenv("BULLY_SETUP_FILE");
  const char *stop = getenv("BULLY_SETUP_STOP");
  if (!setup || !*setup)
    setup = "/tmp/bully_setup.txt";
  if (!stop || !*stop)
    stop = "/tmp/bully_setup_stop";

  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    fprintf(stderr, "[setup] SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }
  SDL_Window *w = SDL_CreateWindow("Bully setup", SDL_WINDOWPOS_UNDEFINED,
                                   SDL_WINDOWPOS_UNDEFINED, 640, 480,
                                   SDL_WINDOW_FULLSCREEN_DESKTOP);
  if (!w) {
    fprintf(stderr, "[setup] window failed: %s\n", SDL_GetError());
    SDL_Quit();
    return 1;
  }
  SDL_Renderer *r = SDL_CreateRenderer(w, -1, SDL_RENDERER_ACCELERATED);
  if (!r)
    r = SDL_CreateRenderer(w, -1, SDL_RENDERER_SOFTWARE);
  if (!r) {
    fprintf(stderr, "[setup] renderer failed: %s\n", SDL_GetError());
    SDL_DestroyWindow(w);
    SDL_Quit();
    return 1;
  }

  while (!file_exists(stop)) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      if (ev.type == SDL_QUIT)
        goto out;
    }

    int state = 1, done = 0, total = 0;
    char msg[180] = "EXTRAINDO DADOS DO APK";
    read_setup_state(setup, &state, &done, &total, msg, sizeof(msg));
    if (total <= 0)
      total = 1;
    if (done < 0)
      done = 0;
    if (done > total)
      done = total;

    int ww = 640, wh = 480;
    SDL_GetRendererOutputSize(r, &ww, &wh);
    SDL_SetRenderDrawColor(r, 4, 7, 12, 255);
    SDL_RenderClear(r);
    SDL_SetRenderDrawColor(r, 236, 238, 224, 255);
    draw_text(r, ww / 2 - 90, wh / 2 - 95, 4, "BULLY V11");
    draw_text(r, ww / 2 - 168, wh / 2 - 42, 2, msg);

    SDL_Rect bar = {ww / 2 - 180, wh / 2 + 8, 360, 22};
    SDL_SetRenderDrawColor(r, 236, 238, 224, 255);
    SDL_RenderDrawRect(r, &bar);
    SDL_Rect fill = {bar.x + 3, bar.y + 3, (bar.w - 6) * done / total, bar.h - 6};
    SDL_SetRenderDrawColor(r, state == 2 ? 180 : 86, state == 2 ? 68 : 174,
                           state == 2 ? 68 : 118, 255);
    SDL_RenderFillRect(r, &fill);

    char prog[64];
    snprintf(prog, sizeof(prog), "%d / %d MB", done, total);
    SDL_SetRenderDrawColor(r, 236, 238, 224, 255);
    draw_text(r, ww / 2 - 72, wh / 2 + 48, 2, prog);
    SDL_RenderPresent(r);
    SDL_Delay(100);
  }

out:
  SDL_DestroyRenderer(r);
  SDL_DestroyWindow(w);
  SDL_Quit();
  return 0;
}

static void load_module(const char *name, int heap_mb, DynLibFunction *tbl, int n) {
  size_t hs = (size_t)heap_mb * 1024 * 1024;
  void *heap = mmap(NULL, hs, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (heap == MAP_FAILED) {
    fprintf(stderr, "mmap %d MB failed for %s\n", heap_mb, name);
    exit(1);
  }

  fprintf(stderr, "== loading %s heap=%p size=%dMB ==\n", name, heap, heap_mb);
  if (so_load(name, heap, hs) < 0 || so_relocate() < 0) {
    fprintf(stderr, "load/relocate failed for %s\n", name);
    exit(1);
  }
  so_resolve(tbl, n, 0);
  so_finalize();
  so_flush_caches();
  so_execute_init_array();
  fprintf(stderr, "== %s text=%p+%zu data=%p+%zu ==\n", name, text_base,
          text_size, data_base, data_size);
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  if (getenv("BULLY_SETUPSPLASH"))
    return run_setup_splash();

  volatile char c = g_bionic_guard_pad[0];
  (void)c;

  install_crash_handler();
  fprintf(stderr, "=== Bully2 original-first so-loader / NextOS Mali-450 ===\n");

  bully_imports_init();
  preload_device_libs();
  build_base_table();

  load_module(CXX_SO, env_mb("BULLY2_CXX_HEAP_MB", CXX_HEAP_MB, 32, 96),
              g_base, g_base_n);
  int cxx_n = 0;
  DynLibFunction *cxx_tbl = so_snapshot_symbols(&cxx_n);
  if (!cxx_tbl || cxx_n <= 0) {
    fprintf(stderr, "libc++ snapshot is empty\n");
    return 1;
  }
  fprintf(stderr, "libc++ exported symbols: %d\n", cxx_n);

  int comb_n = g_base_n + cxx_n;
  DynLibFunction *comb = malloc(sizeof(DynLibFunction) * (size_t)comb_n);
  if (!comb) {
    fprintf(stderr, "combined table malloc failed\n");
    return 1;
  }
  memcpy(comb, g_base, sizeof(DynLibFunction) * (size_t)g_base_n);
  memcpy(comb + g_base_n, cxx_tbl, sizeof(DynLibFunction) * (size_t)cxx_n);

  load_module(GAME_SO,
              env_mb("BULLY2_GAME_HEAP_MB", default_game_heap_mb(), 96, 256),
              comb, comb_n);

  fprintf(stderr, "=== entering JNI lifecycle ===\n");
  jni_load();
  fprintf(stderr, "=== JNI lifecycle returned ===\n");
  return 0;
}
