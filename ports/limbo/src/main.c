/*
 * LIMBO Android NativeActivity bootstrap for NextOS.
 */

#include <SDL2/SDL.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#include "android_shim.h"
#include "egl_shim.h"
#include "error.h"
#include "imports.h"
#include "opensles_shim.h"
#include "so_util.h"
#include "util.h"

#define CXX_SO "libc++_shared.so"
#define SO_NAME "libLimbo.so"
#define MEMORY_MB 384

typedef int (*jni_onload_t)(void *vm, void *reserved);
typedef void (*native_activity_oncreate_t)(ANativeActivity *activity,
                                           void *savedState,
                                           size_t savedStateSize);
typedef int (*limbo_soundengine_init_t)(void *settings, void *platform);

static uintptr_t g_limbo_text = 0;
static size_t g_limbo_text_size = 0;
uintptr_t g_limbo_soundengine_success = 0;
uintptr_t g_limbo_soundengine_fail_resume = 0;
uintptr_t g_limbo_rodata_59000 = 0;

static void crash_handler(int sig, siginfo_t *info, void *uctx) {
  ucontext_t *uc = (ucontext_t *)uctx;
  uintptr_t pc = uc->uc_mcontext.pc;
  uintptr_t fault = (uintptr_t)info->si_addr;

  if (sig == SIGUSR1) {
    long tid = syscall(SYS_gettid);
    fprintf(stderr,
            "\n=== LIMBO TRACE === tid=%ld pc=%p lr=%p sp=%p\n",
            tid, (void *)pc, (void *)uc->uc_mcontext.regs[30],
            (void *)uc->uc_mcontext.sp);
    if (g_limbo_text && pc >= g_limbo_text &&
        pc < g_limbo_text + g_limbo_text_size) {
      fprintf(stderr, "trace pc=libLimbo.so+0x%lx\n",
              (unsigned long)(pc - g_limbo_text));
    }
    uintptr_t lr = (uintptr_t)uc->uc_mcontext.regs[30];
    if (g_limbo_text && lr >= g_limbo_text &&
        lr < g_limbo_text + g_limbo_text_size) {
      fprintf(stderr, "trace lr=libLimbo.so+0x%lx\n",
              (unsigned long)(lr - g_limbo_text));
    }
    uintptr_t sp = (uintptr_t)uc->uc_mcontext.sp;
    for (int i = 0, found = 0; i < 256 && found < 32; i++) {
      uintptr_t v = ((uintptr_t *)sp)[i];
      if (g_limbo_text && v >= g_limbo_text &&
          v < g_limbo_text + g_limbo_text_size) {
        fprintf(stderr, "trace stack[%d]=%p libLimbo.so+0x%lx\n", i,
                (void *)v, (unsigned long)(v - g_limbo_text));
        found++;
      }
    }
    fflush(stderr);
    return;
  }

  if (sig == SIGSEGV && info && info->si_code <= 0) {
    fprintf(stderr,
            "\nLIMBO: ignoring delivered SIGSEGV si_code=%d si_pid=%d si_uid=%d pc=%p lr=%p\n",
            info->si_code, info->si_pid, info->si_uid, (void *)pc,
            (void *)uc->uc_mcontext.regs[30]);
    if (g_limbo_text && pc >= g_limbo_text && pc < g_limbo_text + g_limbo_text_size)
      fprintf(stderr, "delivered pc=libLimbo.so+0x%lx\n",
              (unsigned long)(pc - g_limbo_text));
    uintptr_t sp = (uintptr_t)uc->uc_mcontext.sp;
    for (int i = 0; i < 128; i++) {
      uintptr_t v = ((uintptr_t *)sp)[i];
      if (g_limbo_text && v >= g_limbo_text && v < g_limbo_text + g_limbo_text_size)
        fprintf(stderr, "delivered stack[%d]=%p libLimbo.so+0x%lx\n", i,
                (void *)v, (unsigned long)(v - g_limbo_text));
    }
    return;
  }

  fprintf(stderr, "\n=== LIMBO CRASH ===\n");
  fprintf(stderr, "signal=%d si_code=%d si_pid=%d si_uid=%d fault=%p pc=%p\n",
          sig, info ? info->si_code : 0, info ? info->si_pid : 0,
          info ? info->si_uid : 0, (void *)fault, (void *)pc);
  if (g_limbo_text && pc >= g_limbo_text && pc < g_limbo_text + g_limbo_text_size)
    fprintf(stderr, "pc=libLimbo.so+0x%lx\n", (unsigned long)(pc - g_limbo_text));
  fprintf(stderr, "x0=%016lx x1=%016lx x2=%016lx x3=%016lx\n",
          (unsigned long)uc->uc_mcontext.regs[0],
          (unsigned long)uc->uc_mcontext.regs[1],
          (unsigned long)uc->uc_mcontext.regs[2],
          (unsigned long)uc->uc_mcontext.regs[3]);
  fprintf(stderr, "x29=%016lx x30=%016lx sp=%016lx\n",
          (unsigned long)uc->uc_mcontext.regs[29],
          (unsigned long)uc->uc_mcontext.regs[30],
          (unsigned long)uc->uc_mcontext.sp);

  FILE *maps = fopen("/proc/self/maps", "r");
  if (maps) {
    char line[512];
    uintptr_t lr = (uintptr_t)uc->uc_mcontext.regs[30];
    fprintf(stderr, "maps near pc/lr:\n");
    while (fgets(line, sizeof(line), maps)) {
      unsigned long start = 0, end = 0;
      if (sscanf(line, "%lx-%lx", &start, &end) == 2) {
        if ((pc >= start && pc < end) || (lr >= start && lr < end))
          fprintf(stderr, "%s", line);
      }
    }
    fclose(maps);
  }

  if (g_limbo_text && g_limbo_text_size) {
    uintptr_t sp = (uintptr_t)uc->uc_mcontext.sp;
    uintptr_t *stack = (uintptr_t *)sp;
    int found = 0;
    for (size_t i = 0; i < 512 && found < 32; i++) {
      uintptr_t v = stack[i];
      if (v >= g_limbo_text && v < g_limbo_text + g_limbo_text_size) {
        fprintf(stderr, "stack[%zu]=%p libLimbo.so+0x%lx\n", i, (void *)v,
                (unsigned long)(v - g_limbo_text));
        found++;
      }
    }

    uintptr_t fp = (uintptr_t)uc->uc_mcontext.regs[29];
    for (int depth = 0; depth < 32 && fp; depth++) {
      if (fp < sp || fp - sp > 8u * 1024u * 1024u)
        break;
      uintptr_t next_fp = ((uintptr_t *)fp)[0];
      uintptr_t ret = ((uintptr_t *)fp)[1];
      if (ret >= g_limbo_text && ret < g_limbo_text + g_limbo_text_size) {
        fprintf(stderr, "fp[%d]=%p ret=%p libLimbo.so+0x%lx\n", depth,
                (void *)fp, (void *)ret,
                (unsigned long)(ret - g_limbo_text));
      }
      if (next_fp <= fp || next_fp - fp > 1024u * 1024u)
        break;
      fp = next_fp;
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
  sigaction(SIGABRT, &sa, NULL);
  sigaction(SIGFPE, &sa, NULL);
  sigaction(SIGILL, &sa, NULL);
  sigaction(SIGUSR1, &sa, NULL);
}

static void timeout_exit_handler(int sig) {
  (void)sig;
  const char msg[] = "LIMBO_MAX_SECONDS reached; exiting\n";
  write(STDERR_FILENO, msg, sizeof(msg) - 1);
  _exit(124);
}

static void install_timeout_guard(void) {
  const char *env = getenv("LIMBO_MAX_SECONDS");
  if (!env || !*env)
    return;

  int seconds = atoi(env);
  if (seconds <= 0)
    return;

  signal(SIGALRM, timeout_exit_handler);
  alarm((unsigned)seconds);
  debugPrintf("LIMBO_MAX_SECONDS guard armed: %d seconds\n", seconds);
}

static so_module *load_module(const char *name, size_t mb, int is_aux) {
  size_t heap_size = mb * 1024 * 1024;
  void *heap = mmap(NULL, heap_size, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (heap == MAP_FAILED)
    fatal_error("mmap %s %zu MB", name, mb);

  if (so_load(name, heap, heap_size) < 0)
    fatal_error("so_load %s", name);
  debugPrintf("Loaded %s: text=%p+%zu data=%p+%zu\n",
              name, text_base, text_size, data_base, data_size);
  if (so_relocate() < 0)
    fatal_error("so_relocate %s", name);
  if (so_resolve(dynlib_functions, dynlib_functions_count, 0) < 0)
    fatal_error("so_resolve %s", name);
  so_make_text_writable();
  so_flush_caches();
  so_execute_init_array();

  so_module *m = so_save();
  if (!is_aux) {
    g_limbo_text = (uintptr_t)text_base;
    g_limbo_text_size = text_size;
  }
  return m;
}

static void call_activity_callback(const char *name,
                                   void (*fn)(ANativeActivity *),
                                   ANativeActivity *activity) {
  if (!fn)
    return;
  debugPrintf("NativeActivity: %s\n", name);
  fn(activity);
}

static void lifecycle_tick(void) {
  android_shim_pump_input_frame();
  opensles_shim_pump_callbacks();
  SDL_Delay(30);
}

static void start_lifecycle(struct android_app *host_app) {
  ANativeActivity *activity = host_app->activity;
  ANativeActivityCallbacks *cb = activity->callbacks;
  ANativeWindow *window = android_shim_get_window();

  call_activity_callback("onStart", cb->onStart, activity);
  lifecycle_tick();
  call_activity_callback("onResume", cb->onResume, activity);
  lifecycle_tick();

  if (cb->onInputQueueCreated) {
    debugPrintf("NativeActivity: onInputQueueCreated\n");
    cb->onInputQueueCreated(activity, host_app->inputQueue);
    lifecycle_tick();
  }
  if (cb->onNativeWindowCreated) {
    debugPrintf("NativeActivity: onNativeWindowCreated\n");
    cb->onNativeWindowCreated(activity, window);
    lifecycle_tick();
  }
  if (cb->onNativeWindowResized) {
    debugPrintf("NativeActivity: onNativeWindowResized\n");
    cb->onNativeWindowResized(activity, window);
    lifecycle_tick();
  }
  if (cb->onNativeWindowRedrawNeeded) {
    debugPrintf("NativeActivity: onNativeWindowRedrawNeeded\n");
    cb->onNativeWindowRedrawNeeded(activity, window);
    lifecycle_tick();
  }
  if (cb->onWindowFocusChanged) {
    debugPrintf("NativeActivity: onWindowFocusChanged(1)\n");
    cb->onWindowFocusChanged(activity, 1);
    lifecycle_tick();
  }
}

static void stop_lifecycle(struct android_app *host_app) {
  ANativeActivity *activity = host_app->activity;
  ANativeActivityCallbacks *cb = activity->callbacks;
  ANativeWindow *window = android_shim_get_window();

  if (cb->onWindowFocusChanged)
    cb->onWindowFocusChanged(activity, 0);
  if (cb->onNativeWindowDestroyed)
    cb->onNativeWindowDestroyed(activity, window);
  if (cb->onInputQueueDestroyed)
    cb->onInputQueueDestroyed(activity, host_app->inputQueue);
  if (cb->onPause)
    cb->onPause(activity);
  if (cb->onStop)
    cb->onStop(activity);
  if (cb->onDestroy)
    cb->onDestroy(activity);
}

static void limbo_safe_close_holder(void *obj) {
  uintptr_t p = (uintptr_t)obj;
  if (p < 0x10000) {
    debugPrintf("limbo_safe_close_holder: skip invalid obj=%p\n", obj);
    return;
  }

  void **slot = (void **)(p + 8);
  if (*slot) {
    debugPrintf("limbo_safe_close_holder: clear handle %p from obj=%p\n",
                *slot, obj);
    *slot = NULL;
  }
}

static int limbo_fake_load_bank(void *obj) {
  uintptr_t p = (uintptr_t)obj;
  if (p >= 0x10000) {
    *(uint32_t *)(p + 256) = 1; /* AK_Success */
    *(uint8_t *)(p + 260) = 1;  /* async callback completed */
  }
  debugPrintf("limbo_fake_load_bank: bypass audio bank obj=%p\n", obj);
  return 1;
}

static uintptr_t limbo_read_ptr_field(void *obj, size_t off) {
  uintptr_t p = (uintptr_t)obj;
  uintptr_t v = 0;
  if (p < 0x10000)
    return 0;
  memcpy(&v, (void *)(p + off), sizeof(v));
  return v;
}

static uintptr_t limbo_read_ptr_addr(uintptr_t addr) {
  uintptr_t v = 0;
  if (addr < 0x10000)
    return 0;
  memcpy(&v, (void *)addr, sizeof(v));
  return v;
}

static unsigned long limbo_text_off(uintptr_t addr) {
  uintptr_t base = (uintptr_t)text_base;
  if (!base || addr < base || addr >= base + text_size)
    return 0;
  return (unsigned long)(addr - base);
}

static uint32_t limbo_read_u32_field(void *obj, size_t off) {
  uintptr_t p = (uintptr_t)obj;
  uint32_t v = 0;
  if (p < 0x10000)
    return 0;
  memcpy(&v, (void *)(p + off), sizeof(v));
  return v;
}

static uint16_t limbo_read_u16_field(void *obj, size_t off) {
  uintptr_t p = (uintptr_t)obj;
  uint16_t v = 0;
  if (p < 0x10000)
    return 0;
  memcpy(&v, (void *)(p + off), sizeof(v));
  return v;
}

static uint8_t limbo_read_u8_field(void *obj, size_t off) {
  uintptr_t p = (uintptr_t)obj;
  uint8_t v = 0;
  if (p < 0x10000)
    return 0;
  memcpy(&v, (void *)(p + off), sizeof(v));
  return v;
}

static void limbo_write_u16_field(void *obj, size_t off, uint16_t v) {
  uintptr_t p = (uintptr_t)obj;
  if (p < 0x10000)
    return;
  memcpy((void *)(p + off), &v, sizeof(v));
}

static void limbo_write_u8_field(void *obj, size_t off, uint8_t v) {
  uintptr_t p = (uintptr_t)obj;
  if (p < 0x10000)
    return;
  memcpy((void *)(p + off), &v, sizeof(v));
}

int limbo_soundengine_init_trace(void *settings, void *platform) {
  limbo_soundengine_init_t original =
      (limbo_soundengine_init_t)((uintptr_t)text_base + 0xef45c);
  void *wwise_global = (void *)((uintptr_t)text_base + 0x3dade0);
  uintptr_t audio_slots = (uintptr_t)text_base + 0x3da000;
  void *audio_cfg = (void *)((uintptr_t)text_base + 0x3daae8);
  uintptr_t alloc_base = (uintptr_t)text_base + 0x3da8a0;
  const char *api_env = getenv("LIMBO_AUDIO_API");
  if (api_env && *api_env) {
    uint16_t api = (uint16_t)atoi(api_env);
    limbo_write_u16_field(platform, 104, api);
    debugPrintf("limbo_soundengine_init: override platform+104=%u\n", api);
  }
  if (getenv("LIMBO_FORCE_OPENSL")) {
    limbo_write_u8_field(audio_cfg, 52, 1);
    debugPrintf("limbo_soundengine_init: force audio_cfg+52=1\n");
  }
  debugPrintf("limbo_soundengine_init: pre settings=%p platform=%p "
              "platform+104=%u platform+128=%p platform+136=%p "
              "global+104=%u global+128=%p global+136=%p "
              "audio_cfg=%p cfg+52=%u cfg+104=%u slot2912=%p\n",
              settings, platform, limbo_read_u16_field(platform, 104),
              (void *)limbo_read_ptr_field(platform, 128),
              (void *)limbo_read_ptr_field(platform, 136),
              limbo_read_u16_field(wwise_global, 104),
              (void *)limbo_read_ptr_field(wwise_global, 128),
              (void *)limbo_read_ptr_field(wwise_global, 136), audio_cfg,
              limbo_read_u8_field(audio_cfg, 52),
              limbo_read_u16_field(audio_cfg, 104),
              (void *)limbo_read_ptr_addr(audio_slots + 2912));
  debugPrintf("limbo_soundengine_init: alloc fns +0x3da8a0=%p/+0x%lx "
              "+0x3da8a8=%p/+0x%lx +0x3da8b0=%p/+0x%lx "
              "+0x3da8b8=%p/+0x%lx +0x3da8c0=%p/+0x%lx\n",
              (void *)limbo_read_ptr_addr(alloc_base),
              limbo_text_off(limbo_read_ptr_addr(alloc_base)),
              (void *)limbo_read_ptr_addr(alloc_base + 8),
              limbo_text_off(limbo_read_ptr_addr(alloc_base + 8)),
              (void *)limbo_read_ptr_addr(alloc_base + 16),
              limbo_text_off(limbo_read_ptr_addr(alloc_base + 16)),
              (void *)limbo_read_ptr_addr(alloc_base + 24),
              limbo_text_off(limbo_read_ptr_addr(alloc_base + 24)),
              (void *)limbo_read_ptr_addr(alloc_base + 32),
              limbo_text_off(limbo_read_ptr_addr(alloc_base + 32)));
  int ret = original(settings, platform);
  void *audio_obj = (void *)limbo_read_ptr_addr(audio_slots + 2912);
  g_limbo_audio_obj = (uintptr_t)audio_obj;
  g_limbo_audio_sem = audio_obj ? (uintptr_t)audio_obj + 0x198 : 0;
  debugPrintf("limbo_soundengine_init: post ret=%d global=%p "
              "global+104=%u global+128=%p global+136=%p "
              "cfg+52=%u cfg+104=%u audio_obj=%p\n",
              ret, wwise_global, limbo_read_u16_field(wwise_global, 104),
              (void *)limbo_read_ptr_field(wwise_global, 128),
              (void *)limbo_read_ptr_field(wwise_global, 136),
              limbo_read_u8_field(audio_cfg, 52),
              limbo_read_u16_field(audio_cfg, 104), audio_obj);
  if (audio_obj) {
    uintptr_t vtable = limbo_read_ptr_field(audio_obj, 0);
    debugPrintf("limbo_audio_obj: obj=%p vtable=%p/+0x%lx "
                "list_b0=%p count=%u cap=%u work488=%p work496=%p "
                "work504=%u work508=%u active424=%u\n",
                audio_obj, (void *)vtable, limbo_text_off(vtable),
                (void *)limbo_read_ptr_field(audio_obj, 0xb0),
                limbo_read_u32_field(audio_obj, 0xb8),
                limbo_read_u32_field(audio_obj, 0xbc),
                (void *)limbo_read_ptr_field(audio_obj, 488),
                (void *)limbo_read_ptr_field(audio_obj, 496),
                limbo_read_u32_field(audio_obj, 504),
                limbo_read_u32_field(audio_obj, 508),
                limbo_read_u8_field(audio_obj, 424));
  }
  return ret;
}

__attribute__((naked)) static void limbo_soundengine_init_gate(void) {
  __asm__ volatile(
      "stp x29, x30, [sp, #-16]!\n"
      "bl limbo_soundengine_init_trace\n"
      "ldp x29, x30, [sp], #16\n"
      "cmp w0, #1\n"
      "b.eq 1f\n"
      "adrp x16, g_limbo_rodata_59000\n"
      "ldr x1, [x16, #:lo12:g_limbo_rodata_59000]\n"
      "adrp x16, g_limbo_soundengine_fail_resume\n"
      "ldr x16, [x16, #:lo12:g_limbo_soundengine_fail_resume]\n"
      "br x16\n"
      "1:\n"
      "adrp x16, g_limbo_soundengine_success\n"
      "ldr x16, [x16, #:lo12:g_limbo_soundengine_success]\n"
      "br x16\n");
}

static void patch_arm64_insn(uintptr_t addr, uint32_t insn) {
  *(uint32_t *)addr = insn;
}

static uint32_t arm64_bl(uintptr_t from, uintptr_t to) {
  int64_t delta = (int64_t)to - (int64_t)from;
  return 0x94000000u | ((uint32_t)(delta >> 2) & 0x03ffffffu);
}

static void patch_arm64_bl(uintptr_t from, uintptr_t to) {
  patch_arm64_insn(from, arm64_bl(from, to));
}

static void write_arm64_abs_call_stub(uintptr_t addr, uintptr_t target) {
  uint32_t *p = (uint32_t *)addr;
  p[0] = 0xa9bf7bfdu; /* stp x29, x30, [sp, #-16]! */
  p[1] = 0x580000b1u; /* ldr x17, #20 */
  p[2] = 0xd63f0220u; /* blr x17 */
  p[3] = 0xa8c17bfdu; /* ldp x29, x30, [sp], #16 */
  p[4] = 0xd65f03c0u; /* ret */
  p[5] = 0xd503201fu; /* nop/alignment */
  *(uint64_t *)(p + 6) = target;
}

static void install_limbo_hooks(void) {
  g_limbo_soundengine_success = (uintptr_t)text_base + 0x24d6dc;
  g_limbo_soundengine_fail_resume = (uintptr_t)text_base + 0x24d6c8;
  g_limbo_rodata_59000 = (uintptr_t)text_base + 0x59000;

  hook_arm64((uintptr_t)text_base + 0x1d90e8,
             (uintptr_t)limbo_safe_close_holder);
  /* 0x1d7ef4 is a cleanup helper. It can be called with x0=NULL after a
   * partially failed audio init. Check x20 before the first dereference and
   * skip the nested close-holder call; replacing the later x21 load leaves a
   * stale register once real audio init reaches this cleanup.
   */
  patch_arm64_insn((uintptr_t)text_base + 0x1d7f08, 0xb4000514u);
  patch_arm64_insn((uintptr_t)text_base + 0x1d7f20, 0xd503201fu);
  patch_arm64_insn((uintptr_t)text_base + 0x1d7f24, 0xf9400695u);
  /* The Android-side async dispatcher global is not initialized in this
   * standalone bootstrap. Bank loading reaches this path, and preserving x0
   * leaks the loader poison value as the AK result. Treat the queued operation
   * as accepted instead of dereferencing the missing dispatcher.
   */
  patch_arm64_insn((uintptr_t)text_base + 0xedb34, 0x52800020u);
  patch_arm64_insn((uintptr_t)text_base + 0xedb38, 0x14000003u);
  /* This helper writes through a small Android-side allocator/global that is
   * left as the loader poison value in the standalone port. Several startup
   * logging/state paths call it, so make it report success instead of touching
   * the invalid allocator.
   */
  patch_arm64_insn((uintptr_t)text_base + 0xf3bf8, 0x52800020u);
  patch_arm64_insn((uintptr_t)text_base + 0xf3bfc, 0xd65f03c0u);
  /* Another allocator-backed helper is hit while the engine reports GL/resource
   * state. It normally returns 1 after filling a small Android-managed buffer;
   * in the port that allocator is still the loader poison value, so return the
   * successful result directly.
   */
  patch_arm64_insn((uintptr_t)text_base + 0xf1518, 0x52800020u);
  patch_arm64_insn((uintptr_t)text_base + 0xf151c, 0xd65f03c0u);
  /* Event/telemetry helper for a 0x30-byte payload has the same dependency on
   * the Android-managed queue. It is hit by the GL reporting path before the
   * game reaches visible loading, so accept the event without queuing it.
   */
  patch_arm64_insn((uintptr_t)text_base + 0xeffe0, 0x52800020u);
  patch_arm64_insn((uintptr_t)text_base + 0xeffe4, 0xd65f03c0u);
  /* Small event helper used through wrappers such as 0xf0fa0 follows the same
   * queue path and crashes when the queue global is still poisoned. Return the
   * helper's success code without trying to allocate an event record.
   */
  patch_arm64_insn((uintptr_t)text_base + 0xf0e68, 0x52800020u);
  patch_arm64_insn((uintptr_t)text_base + 0xf0e6c, 0xd65f03c0u);
  /* Companion helper for a 0x14/0x1c-byte event record, reached by the same
   * render analytics code path. It also writes into the poisoned queue global.
   */
  patch_arm64_insn((uintptr_t)text_base + 0xf13d8, 0x52800020u);
  patch_arm64_insn((uintptr_t)text_base + 0xf13dc, 0xd65f03c0u);
  /* The relocation at 0x3d4a58 is named glFinish in this APK, but every sampled
   * call site passes a byte count in x0 and stores the return value as a heap
   * pointer. Resolving it to real glFinish corrupts render/script buffers and
   * eventually crashes in memcpy after GAME ON. Point this single GOT slot at
   * malloc instead of changing the global import table.
   */
  *(uintptr_t *)((uintptr_t)text_base + 0x3d4a58) = (uintptr_t)malloc;
  /* This render/script helper builds a small vector and has several calls that
   * objdump labels as Android/GL imports, but the call sites pass allocation
   * sizes and immediately treat x0 as heap memory. Patch only these local BLs
   * to the APK's malloc PLT so real input/GL imports stay intact.
   */
  uintptr_t malloc_stub = (uintptr_t)text_base + 0x3d6000;
  write_arm64_abs_call_stub(malloc_stub, (uintptr_t)malloc);
  patch_arm64_bl((uintptr_t)text_base + 0x37d4d8, malloc_stub);
  patch_arm64_bl((uintptr_t)text_base + 0x37d4ec, malloc_stub);
  patch_arm64_bl((uintptr_t)text_base + 0x37d500, malloc_stub);
  patch_arm64_bl((uintptr_t)text_base + 0x37d510, malloc_stub);
  patch_arm64_bl((uintptr_t)text_base + 0x37d530, malloc_stub);
  /* Async resource/job registration also assumes an Android-side manager that
   * is NULL before the standalone bootstrap finishes wiring the game. Report
   * "not queued" instead of dereferencing that global.
   */
  patch_arm64_insn((uintptr_t)text_base + 0xf218c, 0x52800000u);
  patch_arm64_insn((uintptr_t)text_base + 0xf2190, 0xd65f03c0u);
  /* Cleanup/refcount helper can be reached with a NULL list after the skipped
   * startup job path. Treat it as already clean.
   */
  patch_arm64_insn((uintptr_t)text_base + 0x10a464, 0x52800020u);
  patch_arm64_insn((uintptr_t)text_base + 0x10a468, 0xd65f03c0u);
  /* When the Wwise sound engine fails to initialize, the error/log path at
   * 0x24db40/0x24db50 try to allocate through an Android-side audio/string
   * helper that is not initialized in the port. Keep the audio failure
   * non-fatal and let the following bank-load bypass handle the missing
   * callback.
   */
  patch_arm64_insn((uintptr_t)text_base + 0x24db40, 0xd503201fu);
  patch_arm64_insn((uintptr_t)text_base + 0x24db50, 0xd503201fu);
  hook_arm64((uintptr_t)text_base + 0x24d6b8,
             (uintptr_t)limbo_soundengine_init_gate);
  if (getenv("LIMBO_FORCE_OPENSL")) {
    /* 0xef700 checks audio_cfg+52 and otherwise selects the non-OpenSL audio
     * path, which never reaches slCreateEngine in this bootstrap. Keep this
     * diagnostic override env-gated while we prove the real backend choice.
     */
    patch_arm64_insn((uintptr_t)text_base + 0xef700, 0xd503201fu);
    /* The OpenSL init method checks a zero-initialized string with
     * basic_string::rfind('\0', 0). On this standalone libc++ state that
     * returns npos and exits with code 2 before slCreateEngine. Skip only this
     * early exit while forcing the OpenSL path.
     */
    patch_arm64_insn((uintptr_t)text_base + 0x1ad650, 0xd503201fu);
    debugPrintf("LIMBO_FORCE_OPENSL: patched audio backend selector/gate\n");
  }
  /* Startup can block forever waiting for a render/job event that the
   * standalone bootstrap never signals. Skip only that startup wait; later
   * event waits are left intact so real render synchronization still runs.
   */
  patch_arm64_insn((uintptr_t)text_base + 0x245dbc, 0xd503201fu);
  /* The APK import labels are shifted for a few local audio/string helpers.
   * These call sites build and hash Wwise bank paths, but currently branch to
   * AInputEvent_getType/read with C-string arguments. Patch the local branches
   * only so normal Android input keeps using the real AInputEvent_getType.
   */
  /* Use the real PLT slots from .rela.plt, not objdump's shifted labels:
   *   strlen GOT 0x3d4990 -> PLT 0x3b3c30
   *   memcpy GOT 0x3d51b0 -> PLT 0x3b4c70
   * The labelled 0x3b3b70/0x3b4e30 slots are printf/dlclose in this APK.
   */
  uintptr_t strlen_plt = (uintptr_t)text_base + 0x3b3c30;
  uintptr_t memcpy_plt = (uintptr_t)text_base + 0x3b4c70;
  patch_arm64_bl((uintptr_t)text_base + 0x111b4c, strlen_plt);
  patch_arm64_bl((uintptr_t)text_base + 0x111b60, strlen_plt);
  patch_arm64_bl((uintptr_t)text_base + 0x111cfc, strlen_plt);
  patch_arm64_bl((uintptr_t)text_base + 0x111d08, strlen_plt);
  patch_arm64_bl((uintptr_t)text_base + 0xeedc8, strlen_plt);
  patch_arm64_bl((uintptr_t)text_base + 0xeef1c, strlen_plt);
  patch_arm64_bl((uintptr_t)text_base + 0xeef44, strlen_plt);
  patch_arm64_bl((uintptr_t)text_base + 0x111b94, memcpy_plt);
  patch_arm64_bl((uintptr_t)text_base + 0x111ba4, memcpy_plt);
  patch_arm64_bl((uintptr_t)text_base + 0x111cc4, memcpy_plt);
  patch_arm64_bl((uintptr_t)text_base + 0xeedf0, memcpy_plt);
  patch_arm64_bl((uintptr_t)text_base + 0xeef38, memcpy_plt);
  /* LIMBO's bank names already include ".bnk" ("init.bnk", "l_intro.bnk").
   * Passing the suffix into the local helper is currently producing
   * "init.bnk.bnk" on this port, so leave the name unchanged here.
   */
  patch_arm64_insn((uintptr_t)text_base + 0xf46e8, 0xaa1f03e2u);
  if (getenv("LIMBO_FAKE_BANKS"))
    hook_arm64((uintptr_t)text_base + 0x26cfc4,
               (uintptr_t)limbo_fake_load_bank);
  /* Bank loading now reaches Wwise, but the Android async completion callback
   * is not pumped by the standalone bootstrap. Keep the real f46b0 load result
   * in w0/[obj+256], then mark the local wait as completed:
   *   26d154: mov w8, #1
   */
  patch_arm64_insn((uintptr_t)text_base + 0x26d154, 0x52800028u);
  so_flush_caches();
  debugPrintf("LIMBO hooks installed\n");
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  install_crash_handler();
  install_timeout_guard();
  setenv("SDL_GAMECONTROLLERCONFIG_FILE", "gamecontrollerdb.txt", 0);

  struct android_app *host_app = android_shim_init();
  egl_shim_create_window();

  so_module *m_cxx = load_module(CXX_SO, 32, 1);
  so_set_aux_module(m_cxx);
  load_module(SO_NAME, MEMORY_MB, 0);
  install_limbo_hooks();

  jni_onload_t JNI_OnLoad = (jni_onload_t)so_find_addr_safe("JNI_OnLoad");
  native_activity_oncreate_t ANativeActivity_onCreate =
      (native_activity_oncreate_t)so_find_addr_safe("ANativeActivity_onCreate");

  debugPrintf("entry: JNI_OnLoad=%p ANativeActivity_onCreate=%p\n",
              (void *)JNI_OnLoad, (void *)ANativeActivity_onCreate);
  if (!ANativeActivity_onCreate)
    fatal_error("ANativeActivity_onCreate not found in %s", SO_NAME);

  if (JNI_OnLoad) {
    int ver = JNI_OnLoad(host_app->activity->vm, NULL);
    debugPrintf("JNI_OnLoad -> 0x%x\n", ver);
  }

  debugPrintf("Calling ANativeActivity_onCreate...\n");
  ANativeActivity_onCreate(host_app->activity, NULL, 0);
  start_lifecycle(host_app);

  while (!host_app->destroyRequested) {
    android_shim_pump_input_frame();
    opensles_shim_pump_callbacks();
    SDL_Delay(5);
  }

  stop_lifecycle(host_app);
  android_shim_cleanup();
  return 0;
}
