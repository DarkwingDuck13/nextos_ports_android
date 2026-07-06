// Audio sink for Ren'Py SDL2. The embedded SDL uses the "android" audio
// driver, which writes PCM via JNI (audioOpen/audioWrite*Buffer). We route that
// PCM to PulseAudio through a `pacat` pipe.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "jni_shim.h"
#include "util.h"

static FILE *g_pa;

// O launcher faz HOME=$GAMEDIR, o que quebra a descoberta do servidor
// PulseAudio pelo pacat (procura em $HOME/.config/pulse) -> conecta em nada
// -> SEM SOM (e o erro fica escondido por "2>/dev/null"). Aqui detectamos o
// socket do pulse do sistema e setamos PULSE_SERVER, que o pacat (via popen)
// herda. Corrige o audio de forma device-agnostica, dentro do binario.
static void ensure_pulse_server(void) {
  if (getenv("PULSE_SERVER"))
    return;
  const char *cands[3];
  int n = 0;
  static char rt[256];
  const char *xrd = getenv("XDG_RUNTIME_DIR");
  if (xrd && *xrd) {
    snprintf(rt, sizeof(rt), "%s/pulse/native", xrd);
    cands[n++] = rt;
  }
  cands[n++] = "/run/pulse/native";
  cands[n++] = "/var/run/pulse/native";
  for (int i = 0; i < n; i++) {
    struct stat st;
    if (stat(cands[i], &st) == 0) {
      char v[300];
      snprintf(v, sizeof(v), "unix:%s", cands[i]);
      setenv("PULSE_SERVER", v, 1);
      debugPrintf("audio: PULSE_SERVER=%s (fix HOME override)\n", v);
      return;
    }
  }
  debugPrintf("audio: nenhum socket pulse encontrado p/ PULSE_SERVER\n");
}

static int audio_open(int sampleRate, int audioFormat, int channels,
                      int desiredFrames) {
  const char *fmt = "s16le";
  if (audioFormat == 3)
    fmt = "u8";
  else if (audioFormat == 4)
    fmt = "float32le";
  /* thread de audio do SDL (quem chama open/write) com prioridade alta:
   * sobrevive aos stalls de CPU dos carregamentos de cena (senao o ring do
   * ALSA underrun-a e REPETE o ultimo trecho = "som em loop"). */
  setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), -12);
  char cmd[256];
  snprintf(cmd, sizeof(cmd),
           "pacat --playback --rate=%d --channels=%d --format=%s "
           "--latency-msec=250 --client-name=summertimesaga 2>/dev/null",
           sampleRate, channels, fmt);
  if (g_pa) {
    pclose(g_pa);
    g_pa = NULL;
  }
  g_pa = popen(cmd, "w");
  debugPrintf("audio_open: rate=%d ch=%d fmt=%s frames=%d pacat=%s\n",
              sampleRate, channels, fmt, desiredFrames, g_pa ? "OK" : "FAIL");
  // This SDL build's audioOpen: 0 == success, non-zero == error.
  return g_pa ? 0 : -1;
}

static void audio_write(void *data, int len_bytes) {
  if (!g_pa || !data || len_bytes <= 0)
    return;
  fwrite(data, 1, (size_t)len_bytes, g_pa);
  fflush(g_pa);
  if (getenv("SUMMERTIME_VERBOSE")) {
    static long n = 0, total = 0;
    total += len_bytes;
    if ((n++ % 200) == 0)
      debugPrintf("audio_write: %ld calls, %ld KB total\n", n, total / 1024);
  }
}

static void audio_close(void) {
  if (g_pa) {
    pclose(g_pa);
    g_pa = NULL;
  }
}

void summertime_audio_init(void) {
  ensure_pulse_server();
  jni_shim_set_audio_cb(audio_open, audio_write, audio_write, audio_close);
  debugPrintf("audio: sink wired (pacat)\n");
}
