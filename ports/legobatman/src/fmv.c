/* fmv.c -- cutscene player stub (Linux port)
 *
 * The engine decodes .mp4 cutscenes via Java (MediaPlayer/SurfaceTexture),
 * which does not exist here. Rather than hang waiting for a movie that never
 * plays, we report every cutscene as finished immediately so the engine skips
 * straight past it into gameplay. (Real ffmpeg playback can be added later.)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include "fmv.h"
#include "util.h"

volatile int g_fmv_active = 0;

void *fmv_hook_open(const char *name, int loop, const void *a, unsigned b, const char *c) {
  (void)loop; (void)a; (void)b; (void)c;
  debugPrintf("FMV: skip cutscene '%s'\n", name ? name : "(null)");
  return (void *)1; // non-NULL dummy handle
}

unsigned char fmv_hook_finished(void *handle) {
  (void)handle;
  return 1; // always finished -> engine advances
}

void fmv_hook_close(void *handle) { (void)handle; }
