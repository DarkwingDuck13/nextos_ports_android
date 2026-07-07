/* config.h -- global configuration and config file handling
 *
 * LEGO Movie Video Game (Android, armeabi-v7a) on Mali-450.
 * The game lib is libLEGO_Emmet.so (WB Games "Fusion" engine, same JNI surface
 * as LEGO Star Wars TFA / Ninjago), driven through the
 * com.wbgames.LEGOgame.Fusion + GameGLSurfaceView JNI surface: classic Android
 * GLSurfaceView model where WE own EGL and call nativeRender() each frame.
 *
 * Unlike TFA/Ninjago (loose Play-Asset-Delivery packs registered via
 * fnOBBPackages_AddAssetDir), this 2016 build ships a single APK-Expansion OBB
 * (a stored .zip). It has NO AddAssetDir; instead the engine reads every subfile
 * by fopen(obb)+fseek(dataOffset). We reproduce that by registering the OBB and
 * each of its central-directory entries at boot (see obb_register in main.c).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __CONFIG_H__
#define __CONFIG_H__

// RWX region reserved for the loaded .so image (image ~4.5 MB + relocations +
// BSS). 128 MB is ample and keeps 32-bit address space usage modest.
#define SO_REGION_MB 128

// armeabi-v7a (softfp) libLEGO_Emmet.so, GLES2. v1.03.1 (com.wb.goog.lego.movievideogame).
#define SO_NAME "libLEGO_Emmet.so"
#define CONFIG_NAME "config.txt"
#define LOG_NAME "debug.log"

// Diagnostic build switch: writes debug.log next to the binary.
// #define DEBUG_LOG 1

// --- Android-side identity handed to the game ------------------------------
// Fusion's nativeSetDeviceStrings takes (MODEL, PRODUCT, MANUFACTURER,
// HARDWARE). Present a generic, well-supported GLES2 phone so the engine takes
// its standard mobile path; the GL renderer is reported separately (glGetString).
#define DEVICE_MODEL        "SM-G950F"
#define DEVICE_PRODUCT      "dreamlte"
#define DEVICE_MANUFACTURER "samsung"
#define DEVICE_HARDWARE     "exynos8895"

#define ANDROID_VERSION_RELEASE "5.0.2"
#define ANDROID_SDK_INT 21

// Audio output buffer (frames) handed to Fusion.nativeSetAudioOutputBufferSize.
#define AUDIO_BUF_FRAMES 256

// --- game data --------------------------------------------------------------
// Single APK-Expansion OBB (stored zip) deployed next to the binary. The engine
// opens subfiles via fnOBBPackages (fopen(obb)+fseek); obb_register() walks the
// zip central directory and feeds every entry to fnOBBPackages_AddFileEntry.
#define PACKAGE  "com.wb.goog.lego.movievideogame"
#define OBB_FILE "main.1031.com.wb.goog.lego.movievideogame.obb"   // relative to the game dir
// Unused here (this build reads from the OBB, not loose assets) but referenced
// by the shared libc_shim.c AAssetManager_open path, which the engine never hits.
#define GAMEDATA_DIR "gamedata"

// --- filesystem paths -------------------------------------------------------
// The engine prefixes save/cache/write paths onto its file opens via "%s/%s".
// fix_path() in libc_shim.c collapses every Android-absolute prefix onto the
// game directory (the process cwd, set to the binary's folder by the launcher).
#define SAVE_PATH   "/data/user/0/com.wb.goog.lego.movievideogame/files"
#define CACHE_PATH  "/data/user/0/com.wb.goog.lego.movievideogame/cache"
#define WRITE_PATH  "/storage/emulated/0/Android/data/com.wb.goog.lego.movievideogame/files"

// Save files the engine opens under SAVE_PATH (fnaFile_SaveGame*):
#define SAVE_GAME_FILE   "savegame.dat"
#define SAVE_CONFIG_FILE "config.dat"

// Physical panel size in mm; the engine uses it for DPI-based UI/touch scaling.
#define SCREEN_PHYS_W_MM 136.7f
#define SCREEN_PHYS_H_MM 76.9f

// actual render/surface size (picked at runtime)
extern int screen_width;
extern int screen_height;

typedef struct {
  int screen_width;
  int screen_height;
  int language;        // -1 = follow system; else index into the lang table
  int show_fps;        // 1 = small FPS counter in the top left corner
} Config;

extern Config config;

int read_config(const char *file);
int write_config(const char *file);

// resolves config.language (or the system language when -1) to a BCP-47 tag
const char *config_locale_str(void);

#endif
