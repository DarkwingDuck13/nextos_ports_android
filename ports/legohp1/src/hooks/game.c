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

void patch_game(void) {
  // cutscene stubs: redirect the engine's fnaFMV_* to our skip player (fmv.c)
  hook_arm(so_try_find_addr(&game_mod, "_Z11fnaFMV_OpenPKcbPK18fnaFMVTRACKCHANNELjS0_"),
           (uintptr_t)&fmv_hook_open);
  hook_arm(so_try_find_addr(&game_mod, "_Z15fnaFMV_FinishedP9FMVHANDLE"),
           (uintptr_t)&fmv_hook_finished);
  hook_arm(so_try_find_addr(&game_mod, "_Z12fnaFMV_CloseP9FMVHANDLE"),
           (uintptr_t)&fmv_hook_close);
  debugPrintf("patch_game: hooked fnaFMV_Open/Finished/Close (skip cutscenes)\n");
}
