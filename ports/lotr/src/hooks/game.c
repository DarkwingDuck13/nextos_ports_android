/* game.c -- binary patches on libLEGOHarry.so (symbol-based, ARM/Thumb)
 *
 * Resolved by exported symbol (the .so keeps its full C++ dynsym). Only bring-up
 * patch: redirect the engine's fnaFMV_* cutscene entry points to our skip stubs
 * (fmv.c) so movies we cannot decode don't hang the boot.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdint.h>
#include <stddef.h>

#include "../util.h"
#include "../so_util.h"
#include "../hooks.h"
#include "../fmv.h"

extern so_module game_mod; // defined in main.c

// TutorialModule_Start(uint,uint) launches the touch-only "tap to move / tap and
// drag" tutorials, which pause gameplay waiting for a touch that never comes on a
// gamepad. Suppress them at the source (same fix as LEGO Batman 2).
static void tutorial_start_stub(unsigned a, unsigned b) { (void)a; (void)b; }

// geReplay = the attract/demo system: after the player is idle for a while the
// game loads a recorded replay and plays it back, driving the character on its
// own ("fica girando/modo defesa"); the exit path checks touch input we don't
// have, so it never stops. Kill it: report never-replaying and no-op the update
// so a demo can never seize control.
static int  replay_isreplaying_stub(void) { return 0; }
static void replay_update_stub(float dt) { (void)dt; }
static void *replay_loadreplay_stub(const char *name) { (void)name; return 0; }

void patch_game(void) {
  // cutscene stubs: redirect the engine's fnaFMV_* to our skip player (fmv.c)
  hook_arm(so_try_find_addr(&game_mod, "_Z11fnaFMV_OpenPKcbPK18fnaFMVTRACKCHANNELjS0_"),
           (uintptr_t)&fmv_hook_open);
  hook_arm(so_try_find_addr(&game_mod, "_Z15fnaFMV_FinishedP9FMVHANDLE"),
           (uintptr_t)&fmv_hook_finished);
  hook_arm(so_try_find_addr(&game_mod, "_Z12fnaFMV_CloseP9FMVHANDLE"),
           (uintptr_t)&fmv_hook_close);

  // kill touch tutorials (they pause the game waiting for a tap)
  hook_arm(so_try_find_addr(&game_mod, "_Z20TutorialModule_Startjj"),
           (uintptr_t)&tutorial_start_stub);

  // kill the idle attract/demo (geReplay) that seizes the character after idle
  hook_arm(so_try_find_addr(&game_mod, "_Z20geReplay_IsReplayingv"),
           (uintptr_t)&replay_isreplaying_stub);
  hook_arm(so_try_find_addr(&game_mod, "_Z15geReplay_Updatef"),
           (uintptr_t)&replay_update_stub);
  hook_arm(so_try_find_addr(&game_mod, "_Z19geReplay_LoadReplayPKc"),
           (uintptr_t)&replay_loadreplay_stub);

  debugPrintf("patch_game: hooked fnaFMV_* + TutorialModule_Start + geReplay (kill attract demo)\n");
}
