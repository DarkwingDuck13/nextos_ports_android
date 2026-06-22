// Audio sink for SOTN. The game's static SDL 2.0.8 uses the "android" audio
// driver, which writes PCM via JNI (audioOpen/audioWriteShortBuffer). We route
// that PCM to PulseAudio through a `pacat` pipe.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "jni_shim.h"
#include "util.h"

static FILE *g_pa;
static int g_bytes_per_frame = 4;

static int audio_open(int sampleRate, int is16Bit, int isStereo,
                      int desiredFrames) {
  int channels = isStereo ? 2 : 1;
  const char *fmt = is16Bit ? "s16le" : "u8";
  g_bytes_per_frame = channels * (is16Bit ? 2 : 1);
  char cmd[256];
  snprintf(cmd, sizeof(cmd),
           "pacat --playback --rate=%d --channels=%d --format=%s "
           "--latency-msec=80 --client-name=sotn 2>/dev/null",
           sampleRate, channels, fmt);
  if (g_pa) {
    pclose(g_pa);
    g_pa = NULL;
  }
  g_pa = popen(cmd, "w");
  debugPrintf("audio_open: rate=%d ch=%d fmt=%s frames=%d pacat=%s\n", sampleRate,
              channels, fmt, desiredFrames, g_pa ? "OK" : "FAIL");
  // This SDL build's audioOpen: 0 == success, non-zero == error.
  return g_pa ? 0 : -1;
}

static void audio_write(void *data, int len_bytes) {
  if (!g_pa || !data || len_bytes <= 0)
    return;
  fwrite(data, 1, (size_t)len_bytes, g_pa);
  fflush(g_pa);
  static long n = 0, total = 0;
  total += len_bytes;
  if ((n++ % 200) == 0)
    debugPrintf("audio_write: %ld calls, %ld KB total\n", n, total / 1024);
}

static void audio_close(void) {
  if (g_pa) {
    pclose(g_pa);
    g_pa = NULL;
  }
}

void sotn_audio_init(void) {
  jni_shim_set_audio_cb(audio_open, audio_write, audio_write, audio_close);
  debugPrintf("audio: sink wired (pacat)\n");
}
