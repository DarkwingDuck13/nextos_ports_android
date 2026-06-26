/*
 * main.c -- entry point for LEGO Batman 3: Beyond Gotham ARM64 Linux port
 *
 * Loads libLEGO_Black_Mobile.so (WB "Fusion" engine), builds a fake
 * Android/JNI environment, and drives the Fusion + GameGLSurfaceView
 * startup sequence.
 *
 * Engine JNI classes:
 *   com.wbgames.LEGOgame.Fusion            -- activity / IO glue
 *   com.wbgames.LEGOgame.GameGLSurfaceView -- GL surface lifecycle / render
 *
 * Based on the lswtcs-src framework (mtojek/initdream). The TTActivity
 * sequence is replaced by the Fusion sequence.
 */

#include <fcntl.h>
#include <linux/fb.h>
#include <pthread.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <ucontext.h>
#include <unistd.h>

#include <SDL2/SDL.h>

#include "android_shim.h"
#include "egl_shim.h"
#include "error.h"
#include "imports.h"
#include "jni_shim.h"
#include "opensles_shim.h"
#include "so_util.h"
#include "util.h"

#define MEMORY_MB 384
#define SO_NAME "libLEGO_Black_Mobile.so"

#define SCREEN_WIDTH 1280
#define SCREEN_HEIGHT 720

/* Where the extracted apk assets (data01, data02, loaded_screen.png, ...)
   live. Overridable via LBBG_DATADIR. */
#define DEFAULT_DATADIR "./assets"
#define DEFAULT_SAVEDIR "./save"

static pthread_t g_main_thread;

/* "thiz" objects handed to every JNI native (distinct sentinels for readable
   crash logs). */
#define FUSION_OBJ    ((void *)0x42420001)
#define GLVIEW_OBJ    ((void *)0x42420002)
#define FAKE_ASSETMGR ((void *)0x24242424)

static char g_datadir[1024];
static char g_savedir[1024];

/* Fake jobjectArray matching jni_shim's FakeObjectArray layout
   ({ jint length; void **elements }). addAssetsDirs / addAPKEntry take a
   String[] which the engine walks via GetArrayLength + GetObjectArrayElement. */
typedef struct { int length; void **elements; } FakeStrArray;
static void *g_assetdir_elems[1];
static FakeStrArray g_assetdir_array = { 1, g_assetdir_elems };

/* ---- Fusion / GameGLSurfaceView native function pointers ---- */
typedef void (*fn_v)(void *, void *);
typedef void (*fn_v_str)(void *, void *, void *);
typedef void (*fn_v_obj)(void *, void *, void *);
typedef void (*fn_v_int)(void *, void *, int);
typedef int  (*fn_i_str)(void *, void *, void *);
typedef void (*fn_v_ii)(void *, void *, int, int);
typedef void (*fn_v_bool)(void *, void *, int);
typedef void (*fn_v_obj2)(void *, void *, void *, void *);
typedef void (*fn_v_5str)(void *, void *, void *, void *, void *, void *, void *);
typedef int  (*fn_int_vm_ptr)(void *, void *);

static struct {
    fn_v_obj  nativeInitializeAssetManager;
    fn_i_str  addAPKEntry;
    fn_i_str  addAssetsDirs;
    fn_v_str  nativeSetSavePath;
    fn_v_str  nativeSetWritePath;
    fn_v_str  nativeSetCachePath;
    fn_v_str  nativeSetCommandLine;
    fn_v_5str nativeSetDeviceStrings;
    fn_v_int  nativeSetAudioOutputBufferSize;

    fn_v_obj2 nativeInit;
    fn_v      nativeColdBoot;
    fn_v_ii   nativeResize;
    fn_v      nativeStart;
    fn_v      nativeResume;
    fn_v      nativePause;
    fn_v      nativeStop;
    fn_v      nativeRender;
    fn_v_bool nativeWindowFocusChanged;
    void (*nativeTouchDown)(void *, void *, int, float, float, float);
    void (*nativeTouchUp)(void *, void *, int, float, float, float);
    void (*nativeTouchGestureStart)(void *, void *, int, float, float, float);
    void (*nativeTouchGestureEnd)(void *, void *, int, float, float, float);
    void (*nativeControllerSetData)(void *, void *, int, int, float, float);
    fn_v  nativeBackButtonPressed;
    /* internal engine input symbols (C++ mangled) — used to force-create and
       poll a JOYPAD input device so the physical controller drives the UI.
       The front-end is otherwise touch-only (no joypad device is registered
       in our boot path). */
    void (*geControls_Init)(void);
    void (*geControls_Update)(float, int);
    /* fnaFMV_SetMovieInfo(bool playing, int frameInfo): writes the global movie
       state byte. The engine WAITS on fnaFMV_Finished() (which is "state byte
       == 0") before leaving a cutscene. We have no video layer, so the byte
       stays non-zero forever -> the game hangs after "New Game" on the intro
       cutscene. Calling SetMovieInfo(0,0) every frame makes every movie report
       finished immediately -> cutscenes are skipped, flow continues to gameplay. */
    void (*fnaFMV_SetMovieInfo)(int, int);
} eng;

static void *resolve_opt(const char *name) {
    uintptr_t a = so_find_addr_safe(name);
    if (!a) debugPrintf("WARN: symbol not found: %s\n", name);
    return (void *)a;
}

static void engine_resolve(void) {
#define R(field, sym) eng.field = (typeof(eng.field))resolve_opt(sym)
    R(nativeInitializeAssetManager, "Java_com_wbgames_LEGOgame_Fusion_nativeInitializeAssetManager");
    R(addAPKEntry,                  "Java_com_wbgames_LEGOgame_Fusion_addAPKEntry");
    R(addAssetsDirs,                "Java_com_wbgames_LEGOgame_Fusion_addAssetsDirs");
    R(nativeSetSavePath,            "Java_com_wbgames_LEGOgame_Fusion_nativeSetSavePath");
    R(nativeSetWritePath,           "Java_com_wbgames_LEGOgame_Fusion_nativeSetWritePath");
    R(nativeSetCachePath,           "Java_com_wbgames_LEGOgame_Fusion_nativeSetCachePath");
    R(nativeSetCommandLine,         "Java_com_wbgames_LEGOgame_Fusion_nativeSetCommandLine");
    R(nativeSetDeviceStrings,       "Java_com_wbgames_LEGOgame_Fusion_nativeSetDeviceStrings");
    R(nativeSetAudioOutputBufferSize,"Java_com_wbgames_LEGOgame_Fusion_nativeSetAudioOutputBufferSize");

    R(nativeInit,               "Java_com_wbgames_LEGOgame_GameGLSurfaceView_nativeInit");
    R(nativeColdBoot,           "Java_com_wbgames_LEGOgame_GameGLSurfaceView_nativeColdBoot");
    R(nativeResize,             "Java_com_wbgames_LEGOgame_GameGLSurfaceView_nativeResize");
    R(nativeStart,              "Java_com_wbgames_LEGOgame_GameGLSurfaceView_nativeStart");
    R(nativeResume,             "Java_com_wbgames_LEGOgame_GameGLSurfaceView_nativeResume");
    R(nativePause,              "Java_com_wbgames_LEGOgame_GameGLSurfaceView_nativePause");
    R(nativeStop,               "Java_com_wbgames_LEGOgame_GameGLSurfaceView_nativeStop");
    R(nativeRender,             "Java_com_wbgames_LEGOgame_GameGLSurfaceView_nativeRender");
    R(nativeWindowFocusChanged, "Java_com_wbgames_LEGOgame_GameGLSurfaceView_nativeWindowFocusChanged");
    R(nativeTouchDown,          "Java_com_wbgames_LEGOgame_Fusion_nativeTouchEventDown");
    R(nativeTouchUp,            "Java_com_wbgames_LEGOgame_Fusion_nativeTouchEventUp");
    R(nativeTouchGestureStart,  "Java_com_wbgames_LEGOgame_Fusion_nativeTouchEventGestureStart");
    R(nativeTouchGestureEnd,    "Java_com_wbgames_LEGOgame_Fusion_nativeTouchEventGestureEnd");
    R(nativeControllerSetData,  "Java_com_wbgames_LEGOgame_Fusion_nativeControllerSetData");
    R(geControls_Init,          "_Z15geControls_Initv");
    R(geControls_Update,        "_Z17geControls_Updatefb");
    R(fnaFMV_SetMovieInfo,      "_Z19fnaFMV_SetMovieInfobi");
    R(nativeBackButtonPressed,  "Java_com_wbgames_LEGOgame_Fusion_nativeBackButtonPressed");
#undef R
    debugPrintf("Fusion: symbols resolved\n");
}

/* ---- generic __stack_chk_fail false-positive workaround ----
   The Fusion engine reads its stack canary from a bionic TLS slot
   ([thread_struct+40]); under our glibc TLS that value can mismatch the
   on-stack copy and trip a *false* __stack_chk_fail. Our non-aborting stub
   then returns and falls through into the next function (e.g. the render
   thread wedges inside StringFrom_eglGetError -> deadlock). Fix: NOP every
   conditional branch that guards a call to __stack_chk_fail@plt. */
#define STACK_CHK_FAIL_PLT_OFF 0x2096c0  /* bl <__stack_chk_fail@plt> target */

static intptr_t sign_extend_bits(uint32_t value, int bits) {
    uint32_t sign_bit = 1u << (bits - 1);
    uint32_t mask = (1u << bits) - 1u;
    value &= mask;
    /* Cast through int32_t so the cast to intptr_t SIGN-extends (a bare
       uint32_t->intptr_t cast would zero-extend, turning negative branch
       displacements into huge positive ones). */
    return (intptr_t)(int32_t)((value ^ sign_bit) - sign_bit);
}
static int decode_b_cond_target(uintptr_t pc, uint32_t insn, uintptr_t *t) {
    if ((insn & 0xff000010u) != 0x54000000u) return 0;
    *t = pc + (sign_extend_bits((insn >> 5) & 0x7ffffu, 19) << 2); return 1;
}
static int decode_cbz_target(uintptr_t pc, uint32_t insn, uintptr_t *t) {
    uint32_t op = insn & 0x7e000000u;
    if (op != 0x34000000u && op != 0x35000000u) return 0;
    *t = pc + (sign_extend_bits((insn >> 5) & 0x7ffffu, 19) << 2); return 1;
}
static uintptr_t decode_bl_target(uintptr_t pc, uint32_t insn) {
    return pc + (sign_extend_bits(insn & 0x03ffffffu, 26) << 2);
}
static void patch_all_stack_chk_branches(void) {
    if (!text_base || text_size < 4) return;
    uintptr_t fail_plt = (uintptr_t)text_base + STACK_CHK_FAIL_PLT_OFF;
    uint32_t *words = (uint32_t *)text_base;
    size_t count = text_size / sizeof(uint32_t);
    int patched = 0, missed = 0, total_bl = 0;
    uintptr_t sample_tgt = 0;
    for (size_t i = 0; i < count; i++) {
        uint32_t insn = words[i];
        uintptr_t pc = (uintptr_t)&words[i];
        if ((insn & 0xfc000000u) != 0x94000000u) continue;     /* BL */
        total_bl++;
        uintptr_t bltgt = decode_bl_target(pc, insn);
        if (pc == (uintptr_t)text_base + 0x3d5614) sample_tgt = bltgt; /* known canary BL */
        if (bltgt != fail_plt) continue;
        int found = 0;
        for (size_t back = 1; back <= 16 && back <= i; back++) {
            uintptr_t bt = 0, bpc = (uintptr_t)&words[i - back];
            uint32_t bi = words[i - back];
            if (!decode_b_cond_target(bpc, bi, &bt) &&
                !decode_cbz_target(bpc, bi, &bt))
                continue;
            if (bt == pc) { words[i - back] = 0xd503201f; patched++; found = 1; break; }
        }
        if (!found) missed++;
    }
    debugPrintf("Patch: NOP'd %d stack-chk branches (missed %d) total_bl=%d "
                "fail_plt=%p known_bl_tgt=%p\n",
                patched, missed, total_bl, (void *)fail_plt, (void *)sample_tgt);
}

/* ---- crash handler ---- */
static void crash_handler(int sig, siginfo_t *info, void *uctx) {
    ucontext_t *uc = (ucontext_t *)uctx;
    uintptr_t pc = uc->uc_mcontext.pc;
    uintptr_t text = (uintptr_t)text_base;
    fprintf(stderr, "\n=== CRASH === sig=%d fault=%p pc=%p\n",
            sig, info->si_addr, (void *)pc);
    if (pc >= text && pc < text + text_size)
        fprintf(stderr, "PC in %s+0x%lx\n", SO_NAME, (unsigned long)(pc - text));
    else
        fprintf(stderr, "PC outside %s\n", SO_NAME);
    for (int i = 0; i < 31; i++) {
        fprintf(stderr, " x%-2d=0x%016lx", i, (unsigned long)uc->uc_mcontext.regs[i]);
        if (i % 3 == 2) fprintf(stderr, "\n");
    }
    fprintf(stderr, "\n sp=0x%016lx pc=0x%016lx\n",
            (unsigned long)uc->uc_mcontext.sp, (unsigned long)pc);
    uintptr_t fp = uc->uc_mcontext.regs[29];
    fprintf(stderr, "Backtrace:\n");
    for (int frame = 0; frame < 24 && fp; frame++) {
        uintptr_t *p = (uintptr_t *)fp;
        uintptr_t lr = p[1];
        if (!lr) break;
        fprintf(stderr, "  #%-2d lr %p", frame, (void *)lr);
        if (lr >= text && lr < text + text_size)
            fprintf(stderr, " (%s+0x%lx)", SO_NAME, (unsigned long)(lr - text));
        fprintf(stderr, "\n");
        if (p[0] <= fp) break;
        fp = p[0];
    }
    fprintf(stderr, "text_base=%p size=0x%zx\n", text_base, text_size);
    fflush(stderr);
    _exit(128 + sig);
}
static void install_crash_handler(void) {
    struct sigaction sa; memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler; sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL); sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGILL,  &sa, NULL);
}

/* ---- gamepad ---- */
static SDL_GameController *g_controller = NULL;
static int g_request_quit = 0;

static void open_controller(void) {
    int n = SDL_NumJoysticks();
    for (int i = 0; i < n; i++) {
        if (SDL_IsGameController(i)) {
            g_controller = SDL_GameControllerOpen(i);
            if (g_controller) {
                debugPrintf("Controller: %s\n", SDL_GameControllerName(g_controller));
                return;
            }
        }
    }
}

/* Dedicated keeper: SDL renders into the lower half of the 1280x1440 fbdev and
   something keeps resetting the visible region to the top (black). A small
   thread re-asserts the pan so the rendered frame stays on screen. */
static void *pan_keeper(void *arg) {
    (void)arg;
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) return NULL;
    struct fb_var_screeninfo vi;
    for (;;) {
        if (ioctl(fd, FBIOGET_VSCREENINFO, &vi) == 0 &&
            vi.yres_virtual >= (unsigned)(SCREEN_HEIGHT * 2) &&
            vi.yoffset != (unsigned)SCREEN_HEIGHT) {
            vi.yoffset = SCREEN_HEIGHT;
            ioctl(fd, FBIOPAN_DISPLAY, &vi);
        }
        usleep(8000);
    }
    return NULL;
}

static void ensure_dir(const char *p) {
    char tmp[1024]; snprintf(tmp, sizeof(tmp), "%s", p);
    for (char *s = tmp + 1; *s; s++) {
        if (*s == '/') { *s = 0; mkdir(tmp, 0775); *s = '/'; }
    }
    mkdir(tmp, 0775);
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    g_main_thread = pthread_self();

    debugPrintf("=== LEGO Batman 3 (Fusion) ARM64 Mali-450 port ===\n");

    const char *dd = getenv("LBBG_DATADIR");
    snprintf(g_datadir, sizeof(g_datadir), "%s", dd ? dd : DEFAULT_DATADIR);
    const char *sd = getenv("LBBG_SAVEDIR");
    snprintf(g_savedir, sizeof(g_savedir), "%s", sd ? sd : DEFAULT_SAVEDIR);
    ensure_dir(g_savedir);
    android_shim_set_data_path(g_datadir);
    debugPrintf("datadir=%s savedir=%s\n", g_datadir, g_savedir);

    /* Index the apkvision asset archives (STORED zips) so AAssetManager_open
       can serve game files directly by offset (no extraction). */
    {
        char zp[1100];
        snprintf(zp, sizeof(zp), "%s/data01", g_datadir); android_shim_add_zip(zp);
        snprintf(zp, sizeof(zp), "%s/data02", g_datadir); android_shim_add_zip(zp);
    }

    size_t heap_size = (size_t)MEMORY_MB * 1024 * 1024;
    void *heap = mmap(NULL, heap_size, PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (heap == MAP_FAILED) fatal_error("heap mmap failed");
    debugPrintf("heap=%p (%d MB)\n", heap, MEMORY_MB);

    debugPrintf("Loading %s...\n", SO_NAME);
    if (so_load(SO_NAME, heap, heap_size) < 0) fatal_error("so_load failed");
    debugPrintf("Loaded: text=%p+%zu data=%p+%zu\n",
                text_base, text_size, data_base, data_size);

    debugPrintf("Relocating...\n");
    if (so_relocate() < 0) fatal_error("so_relocate failed");
    debugPrintf("Resolving %zu imports...\n", dynlib_numfunctions);
    if (so_resolve(dynlib_functions, dynlib_numfunctions, 0) < 0)
        fatal_error("so_resolve failed");

    patch_all_stack_chk_branches();

    /* Sound is disabled via the engine's own no-audio path (slCreateEngine
       returns failure in opensles_shim), so we no longer patch the sound
       resource loop or stub playback functions here. */

    /* fnaDevice_AndroidNative_GetSystemLanguage (+0x2382d0) queries Java via a
       cached JNIEnv we don't populate, dereferencing null. We force English
       anyway, so stub it to `mov w0,#0; ret` (language 0 = English/default). */
    if (text_base) {
        uint32_t *gl = (uint32_t *)((uintptr_t)text_base + 0x2382d0);
        gl[0] = 0x52800000; /* mov w0, #0 */
        gl[1] = 0xd65f03c0; /* ret */
        debugPrintf("Patch: GetSystemLanguage stubbed -> 0 (English)\n");
    }

    so_finalize();
    so_flush_caches();

    debugPrintf("init_array...\n");
    so_execute_init_array();

    debugPrintf("SDL_Init...\n");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0)
        fatal_error("SDL_Init: %s", SDL_GetError());
    egl_shim_create_window();
    install_crash_handler();

    jni_shim_init();

    /* The engine has no JNI_OnLoad; it caches its JNIEnv in a global
       (libLEGO+0x5d4ca0) that stays NULL here, so fnJNI_Global_FindClassAndMethod
       (e.g. GetSystemLanguage) dereferences null. Point it at our fake env. */
    if (text_base)
        *(void **)((uintptr_t)text_base + 0x5d4ca0) = &g_jni_env;

    engine_resolve();

    fn_int_vm_ptr jni_onload = (fn_int_vm_ptr)so_find_addr_safe("JNI_OnLoad");
    if (jni_onload) {
        int v = jni_onload(&g_jni_vm, NULL);
        debugPrintf("JNI_OnLoad -> 0x%x\n", v);
    }

    void *env = &g_jni_env;

    /* ---- Fusion IO setup ---- */
    if (eng.nativeInitializeAssetManager)
        eng.nativeInitializeAssetManager(env, FUSION_OBJ, FAKE_ASSETMGR);
    g_assetdir_elems[0] = (void *)g_datadir;
    if (eng.addAssetsDirs) {
        int r = eng.addAssetsDirs(env, FUSION_OBJ, &g_assetdir_array);
        debugPrintf("addAssetsDirs([%s]) -> %d\n", g_datadir, r);
    }
    if (eng.addAPKEntry) {
        int r = eng.addAPKEntry(env, FUSION_OBJ, &g_assetdir_array);
        debugPrintf("addAPKEntry([%s]) -> %d\n", g_datadir, r);
    }
    if (eng.nativeSetSavePath)  eng.nativeSetSavePath(env, FUSION_OBJ, (void *)g_savedir);
    if (eng.nativeSetWritePath) eng.nativeSetWritePath(env, FUSION_OBJ, (void *)g_savedir);
    if (eng.nativeSetCachePath) eng.nativeSetCachePath(env, FUSION_OBJ, (void *)g_savedir);
    if (eng.nativeSetCommandLine) eng.nativeSetCommandLine(env, FUSION_OBJ, (void *)"");
    if (eng.nativeSetDeviceStrings)
        eng.nativeSetDeviceStrings(env, FUSION_OBJ,
            (void *)"Trimui", (void *)"Smart Pro",
            (void *)"en_US", (void *)"5.0.2", (void *)"arm64-v8a");
    if (eng.nativeSetAudioOutputBufferSize)
        eng.nativeSetAudioOutputBufferSize(env, FUSION_OBJ, 2048);

    /* ---- GL lifecycle ----
       The FIRST nativeRender runs Fusion_OnceInit (the engine master init:
       memory, threads, sound, render). So nativeStart (which does the sound
       *resource* init and assumes the sound subsystem already exists) and the
       resume/focus calls must come AFTER that first frame, not before. */
    if (eng.nativeInit) eng.nativeInit(env, GLVIEW_OBJ, FAKE_ASSETMGR, NULL);
    if (eng.nativeColdBoot) eng.nativeColdBoot(env, GLVIEW_OBJ);
    if (eng.nativeResize) eng.nativeResize(env, GLVIEW_OBJ, SCREEN_WIDTH, SCREEN_HEIGHT);

    /* First frame: triggers Fusion_OnceInit (master init). */
    if (eng.nativeRender) eng.nativeRender(env, GLVIEW_OBJ);
    debugPrintf("first nativeRender done (Fusion_OnceInit ran)\n");

    if (eng.nativeStart) eng.nativeStart(env, GLVIEW_OBJ);
    if (eng.nativeResume) eng.nativeResume(env, GLVIEW_OBJ);
    if (eng.nativeWindowFocusChanged) eng.nativeWindowFocusChanged(env, GLVIEW_OBJ, 1);
    debugPrintf("nativeStart/Resume/Focus done\n");

    /* Force-create a JOYPAD input device so the physical controller drives the
       UI. Normally geControls_Init() runs inside geMain_InitGame(); if our boot
       path skips it the front-end has no joypad device and ignores the pad. */
    if (getenv("LBBG_GECTRL") && eng.geControls_Init) {
        debugPrintf("calling geControls_Init() to register joypad device\n");
        eng.geControls_Init();
    }

    debugPrintf("=== entering render loop ===\n");
    open_controller();
    SDL_GameControllerEventState(SDL_ENABLE);

    /* pan_keeper forces yoffset to the lower half for the single-buffer present
       path. In native double-buffer mode SDL owns the flip, so the keeper would
       fight it and tear -> skip it. */
    /* pan_keeper only needed for the legacy single-buffer pan path. FBCOPY (now
       default) keeps the display on the upper half itself. */
    if (getenv("LBBG_NOFBCOPY") && !getenv("LBBG_DBLBUF")) { pthread_t pk; pthread_create(&pk, NULL, pan_keeper, NULL); }

    int running = 1;
    unsigned long frame = 0;
    int tap_phase = -1;
    while (running) {
        frame++;
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = 0;
            else if (ev.type == SDL_CONTROLLERDEVICEADDED && !g_controller) open_controller();
        }

        /* Build the Fusion controller bitmask from the SDL gamepad and feed it
           every frame. Bit layout is being mapped empirically (LBBG_BTNTEST
           lets us probe). Also auto-pulse a "confirm" for the first few seconds
           to get past the TT splash / "tap to continue" screen. */
        (void)tap_phase;
        if (eng.nativeControllerSetData) {
            int mask = 0;
            float lx = 0, ly = 0;
            if (g_controller) {
                SDL_GameControllerUpdate();
                struct { int sdl; int bit; } bm[] = {
                    { SDL_CONTROLLER_BUTTON_A, 4 }, { SDL_CONTROLLER_BUTTON_B, 5 },
                    { SDL_CONTROLLER_BUTTON_X, 6 }, { SDL_CONTROLLER_BUTTON_Y, 7 },
                    { SDL_CONTROLLER_BUTTON_START, 8 }, { SDL_CONTROLLER_BUTTON_BACK, 9 },
                    { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 10 }, { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 11 },
                    { SDL_CONTROLLER_BUTTON_DPAD_UP, 2 }, { SDL_CONTROLLER_BUTTON_DPAD_DOWN, 3 },
                    { SDL_CONTROLLER_BUTTON_DPAD_LEFT, 12 }, { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 13 },
                };
                for (size_t i = 0; i < sizeof(bm)/sizeof(bm[0]); i++)
                    if (SDL_GameControllerGetButton(g_controller, bm[i].sdl)) mask |= (1 << bm[i].bit);
                int rlx = SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_LEFTX);
                int rly = SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_LEFTY);
                if (rlx > 8000 || rlx < -8000) lx = rlx / 32767.0f;
                if (rly > 8000 || rly < -8000) ly = rly / 32767.0f;
            }
            /* Button-bit sweep: hold exactly one bit (0..13) for ~8s, cycling,
               to discover which bit the front-end treats as "select/confirm".
               Logs the active bit each time it changes. (LBBG_BTNSWEEP) */
            if (getenv("LBBG_BTNSWEEP") && frame >= 600) {
                static int g_sweep_last = -1;
                int phase = (int)((frame - 600) / 480);   /* 480 frames ~= 8s */
                int bit = phase % 14;
                int sub = (int)((frame - 600) % 480);
                if (sub < 420) mask |= (1 << bit);          /* hold 7s, rest 1s */
                if (bit != g_sweep_last && sub == 0) {
                    debugPrintf(">>> SWEEP active bit=%d (phase=%d)\n", bit, phase);
                    fflush(stdout);
                    /* also drop a one-line marker we can cat anytime */
                    FILE *m = fopen("/tmp/lbbg_sweep", "w");
                    if (m) { fprintf(m, "bit=%d phase=%d\n", bit, phase); fclose(m); }
                    g_sweep_last = bit;
                }
            }
            /* Live bit probe: hold whatever bit number is in /tmp/lbbg_btn (and
               optionally a left-stick direction via /tmp/lbbg_axis "x y"). Lets us
               discover the real button-bit layout empirically against the clean
               render. (LBBG_BTNFILE) Write -1 to clear. */
            if (getenv("LBBG_BTNFILE")) {
                FILE *bf = fopen("/tmp/lbbg_btn", "r");
                if (bf) { int b=-1; if (fscanf(bf,"%d",&b)==1 && b>=0 && b<32) mask |= (1<<b); fclose(bf); }
                FILE *af = fopen("/tmp/lbbg_axis", "r");
                if (af) { float ax=0,ay=0; if (fscanf(af,"%f %f",&ax,&ay)==2){lx=ax;ly=ay;} fclose(af); }
            }
            eng.nativeControllerSetData(env, FUSION_OBJ, 0, mask, lx, ly);
        }
        /* CONTROLLER -> TOUCH shim for the touch-only front-end menu. On NextOS's
           handheld there is no touchscreen, so map the physical pad to synthetic
           taps at the menu hotspots: A/Start -> play button (New Game / confirm).
           Edge-triggered (fire once per press) via a clean GestureStart->TouchUp->
           GestureEnd sequence spread over a few frames. (LBBG_PADTAP) */
        if (getenv("LBBG_PADTAP") && g_controller &&
            eng.nativeTouchGestureStart && eng.nativeTouchGestureEnd) {
            static int prev_confirm = 0, seq = -1;
            static float sx = 390, sy = 360;
            SDL_GameControllerUpdate();
            int confirm = SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_A) ||
                          SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_START);
            if (confirm && !prev_confirm && seq < 0) { seq = 0; sx = 390; sy = 360; }
            prev_confirm = confirm;
            if (seq >= 0) {
                if (seq == 0)  eng.nativeTouchGestureStart(env, FUSION_OBJ, 0, sx, sy, 1.0f);
                if (seq == 6 && eng.nativeTouchUp)
                               eng.nativeTouchUp(env, FUSION_OBJ, 0, sx, sy, 0.0f);
                if (seq == 8) { eng.nativeTouchGestureEnd(env, FUSION_OBJ, 0, sx, sy, 0.0f); seq = -1; }
                else seq++;
            }
        }
        /* File-triggered ONE-SHOT tap: when /tmp/lbbg_tap exists, read "x y" from
           it (or use LBBG_TAPX/Y / play-button default), perform a single gesture
           tap, and remove the file. Lets us tap with exact timing/coords from the
           outside without the oscillation of a periodic tap. (LBBG_FILETAP) */
        if (getenv("LBBG_FILETAP") && eng.nativeTouchGestureStart && eng.nativeTouchGestureEnd) {
            static int g_tapfr = 0;          /* frame at which a queued tap began */
            static float g_tx = 380, g_ty = 360;
            if (g_tapfr == 0) {
                FILE *tf = fopen("/tmp/lbbg_tap", "r");
                if (tf) {
                    float x, y;
                    if (fscanf(tf, "%f %f", &x, &y) == 2) { g_tx = x; g_ty = y; }
                    fclose(tf);
                    remove("/tmp/lbbg_tap");
                    g_tapfr = (int)frame;
                    debugPrintf(">>> FILETAP at (%.0f,%.0f) frame=%lu\n", g_tx, g_ty, frame);
                    fflush(stdout);
                }
            } else {
                int d = (int)frame - g_tapfr;
                if (d == 0)  eng.nativeTouchGestureStart(env, FUSION_OBJ, 0, g_tx, g_ty, 1.0f);
                if (d == 8 && eng.nativeTouchUp)
                             eng.nativeTouchUp(env, FUSION_OBJ, 0, g_tx, g_ty, 0.0f);
                if (d == 10) eng.nativeTouchGestureEnd(env, FUSION_OBJ, 0, g_tx, g_ty, 0.0f);
                if (d >= 10) g_tapfr = 0;    /* ready for the next queued tap */
            }
        }
        if (getenv("LBBG_AUTOTAP") && eng.nativeTouchGestureStart && eng.nativeTouchGestureEnd) {
            /* Periodic synthetic tap, every ~1.5s after frame 180. The engine's
               own Android glue brackets every touch with GestureStart (which calls
               ResetData then registers the finger-down) and GestureEnd (ReleaseAll).
               A bare Down/Up without that bracket is never framed/flushed, so use
               the gesture pair. Default target is the front-end "play / New Game"
               button (~380,360 in 1280x720); override with LBBG_TAPX / LBBG_TAPY. */
            const char *txs = getenv("LBBG_TAPX"), *tys = getenv("LBBG_TAPY");
            const char *evs = getenv("LBBG_TAPEVERY");
            float tx = txs ? (float)atoi(txs) : 380.0f;
            float ty = tys ? (float)atoi(tys) : 360.0f;
            int every = evs ? atoi(evs) : 90;
            if (every < 20) every = 20;
            int ph = frame % every;
            /* tap = GestureStart (reset+down) -> TouchUp (sets the per-point
               "released" bit the button-click detection needs) -> GestureEnd. */
            if (frame >= 180 && ph == 0)
                eng.nativeTouchGestureStart(env, FUSION_OBJ, 0, tx, ty, 1.0f);
            if (frame >= 180 && ph == 8 && eng.nativeTouchUp)
                eng.nativeTouchUp(env, FUSION_OBJ, 0, tx, ty, 0.0f);
            if (frame >= 180 && ph == 10)
                eng.nativeTouchGestureEnd(env, FUSION_OBJ, 0, tx, ty, 0.0f);
        }
        /* SELECT+START quits (PortMaster convention) */
        if (g_controller) {
            SDL_GameControllerUpdate();
            if (SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_BACK) &&
                SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_START))
                running = 0;
        }
        /* Skip FMV/cutscene videos (we have no video layer): force "movie
           finished" so the engine never hangs waiting for a video to end. */
        if (!getenv("LBBG_NOSKIPFMV") && eng.fnaFMV_SetMovieInfo)
            eng.fnaFMV_SetMovieInfo(0, 0);
        if (eng.nativeRender) eng.nativeRender(env, GLVIEW_OBJ);
        /* Presentation normally happens on this (render) thread via the glClear
           hook. But the front-end doesn't always clear the default framebuffer
           every frame (it relies on the Android video layer behind), so the hook
           may not fire -> drive the software-double-buffer copy here too. This
           thread owns the GL context, so glFinish in the copy is valid. */
        if (!getenv("LBBG_NOFBCOPY")) egl_shim_present();
        opensles_shim_pump_callbacks();
        if (g_request_quit) running = 0;
        SDL_Delay(16); /* ~60 fps so the synthetic-tap cadence is sane */
    }

    if (eng.nativePause) eng.nativePause(env, GLVIEW_OBJ);
    if (eng.nativeStop) eng.nativeStop(env, GLVIEW_OBJ);
    if (g_controller) SDL_GameControllerClose(g_controller);
    SDL_Quit();
    _exit(0);
}
