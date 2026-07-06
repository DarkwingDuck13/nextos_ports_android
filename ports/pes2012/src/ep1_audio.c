#include "ep1_audio.h"

#include <SDL2/SDL.h>
#include <errno.h>
#include <mpg123.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "util.h"

#define OUT_RATE 44100
#define OUT_CH 2
#define EP1_AUDIO_SAMPLES 1024
#define MAX_AUDIO_CACHE 32
#define MAX_AUDIO_VOICES 12

typedef struct {
  int16_t *pcm;
  uint32_t frames;
  int duration_ms;
} AudioBuffer;

typedef struct {
  char path[256];
  char name[160];
  long long offset;
  long long size;
  AudioBuffer *buf;
} AudioCache;

typedef struct {
  int handle;
  AudioBuffer *buf;
  uint32_t pos;
  float volume;
  int loop;
  int paused;
  int active;
} AudioVoice;

static pthread_mutex_t g_audio_lock = PTHREAD_MUTEX_INITIALIZER;
static SDL_AudioDeviceID g_audio_dev;
static int g_audio_ready;
static int g_mpg123_ready;
static int g_next_handle = 1;
static int g_gameplay_music_active;
static int g_menu_music_active;
static int g_title_music_active;
static int g_sfx_active;
static void *g_sfx_env;
static void *g_sfx_obj;
static ep1_generate_audio_fn g_sfx_generate;
static ep1_make_short_array_fn g_sfx_make_array;
static AudioCache g_cache[MAX_AUDIO_CACHE];
static AudioVoice g_voices[MAX_AUDIO_VOICES];

static int audio_log_enabled(void) {
  static int cached = -1;
  if (cached < 0)
    cached = getenv("SONIC4EP1_AUDIOLOG") ? 1 : 0;
  return cached;
}

static int16_t clamp_s16(int v) {
  if (v > 32767)
    return 32767;
  if (v < -32768)
    return -32768;
  return (int16_t)v;
}

static void audio_cb(void *userdata, Uint8 *stream, int len) {
  (void)userdata;
  memset(stream, 0, (size_t)len);
  int16_t *dst = (int16_t *)stream;
  int frames = len / (int)(sizeof(int16_t) * OUT_CH);

  pthread_mutex_lock(&g_audio_lock);
  for (int v = 0; v < MAX_AUDIO_VOICES; v++) {
    AudioVoice *voice = &g_voices[v];
    if (!voice->active || voice->paused || !voice->buf || !voice->buf->pcm)
      continue;

    for (int f = 0; f < frames; f++) {
      if (voice->pos >= voice->buf->frames) {
        if (voice->loop)
          voice->pos = 0;
        else {
          voice->active = 0;
          break;
        }
      }

      const int16_t *src = voice->buf->pcm + voice->pos * OUT_CH;
      int di = f * OUT_CH;
      int l = dst[di + 0] + (int)((float)src[0] * voice->volume);
      int r = dst[di + 1] + (int)((float)src[1] * voice->volume);
      dst[di + 0] = clamp_s16(l);
      dst[di + 1] = clamp_s16(r);
      voice->pos++;
    }
  }
  pthread_mutex_unlock(&g_audio_lock);

  if (g_sfx_active && g_sfx_generate && g_sfx_make_array) {
    int samples = frames * OUT_CH;
    int16_t tmp[EP1_AUDIO_SAMPLES * OUT_CH];
    if (samples > (int)(sizeof(tmp) / sizeof(tmp[0])))
      samples = (int)(sizeof(tmp) / sizeof(tmp[0]));
    memset(tmp, 0, (size_t)samples * sizeof(tmp[0]));
    void *arr = g_sfx_make_array(tmp, samples);
    g_sfx_generate(g_sfx_env, g_sfx_obj, arr, samples);

    int peak = 0;
    for (int i = 0; i < samples; i++) {
      int av = tmp[i] < 0 ? -tmp[i] : tmp[i];
      if (av > peak)
        peak = av;
      int mixed = dst[i] + (int)((float)tmp[i] * 0.80f);
      dst[i] = clamp_s16(mixed);
    }
    if (peak > 0 && audio_log_enabled()) {
      static int peak_logs;
      if (peak_logs < 40) {
        debugPrintf("ep1_audio: SFX peak=%d samples=%d\n", peak, samples);
        peak_logs++;
      }
    }
  }
}

static int ensure_audio(void) {
  if (g_audio_ready)
    return 1;
  if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
    debugPrintf("ep1_audio: SDL audio init falhou: %s\n", SDL_GetError());
    return 0;
  }

  SDL_AudioSpec want;
  memset(&want, 0, sizeof(want));
  want.freq = OUT_RATE;
  want.format = AUDIO_S16SYS;
  want.channels = OUT_CH;
  want.samples = EP1_AUDIO_SAMPLES;
  want.callback = audio_cb;

  SDL_AudioSpec have;
  memset(&have, 0, sizeof(have));
  g_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
  if (!g_audio_dev) {
    debugPrintf("ep1_audio: SDL_OpenAudioDevice falhou: %s\n", SDL_GetError());
    return 0;
  }
  if (have.freq != OUT_RATE || have.channels != OUT_CH ||
      have.format != AUDIO_S16SYS) {
    debugPrintf("ep1_audio: formato inesperado %d Hz ch=%d fmt=0x%x\n",
                have.freq, have.channels, have.format);
  }
  SDL_PauseAudioDevice(g_audio_dev, 0);
  g_audio_ready = 1;
  debugPrintf("ep1_audio: device aberto %d Hz ch=%d\n", have.freq,
              have.channels);
  return 1;
}

static uint16_t rd16(const unsigned char *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static void resolve_zip_entry_name(const char *path, long long offset,
                                   long long size, char *out,
                                   size_t out_size) {
  snprintf(out, out_size, "apk:%lld+%lld", offset, size);
  FILE *f = fopen(path, "rb");
  if (!f)
    return;

  long long pos = 0;
  unsigned char hdr[30];
  while (fseeko(f, (off_t)pos, SEEK_SET) == 0 &&
         fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr)) {
    uint32_t sig = rd32(hdr);
    if (sig == 0x02014b50 || sig == 0x06054b50)
      break;
    if (sig != 0x04034b50) {
      pos++;
      continue;
    }

    uint32_t comp_size = rd32(hdr + 18);
    uint16_t name_len = rd16(hdr + 26);
    uint16_t extra_len = rd16(hdr + 28);
    long long data_off = pos + 30 + name_len + extra_len;
    char name[160] = {0};
    if (name_len > 0) {
      size_t n = name_len < sizeof(name) - 1 ? name_len : sizeof(name) - 1;
      fread(name, 1, n, f);
      name[n] = 0;
    }
    if (data_off == offset && (long long)comp_size == size) {
      snprintf(out, out_size, "%s", name[0] ? name : out);
      break;
    }
    pos = data_off + comp_size;
  }
  fclose(f);
}

static unsigned char *read_segment(const char *path, long long offset,
                                   long long size) {
  if (!path || offset < 0 || size <= 0 || size > 16 * 1024 * 1024)
    return NULL;
  FILE *f = fopen(path, "rb");
  if (!f) {
    debugPrintf("ep1_audio: fopen(%s) falhou: %s\n", path, strerror(errno));
    return NULL;
  }
  unsigned char *data = malloc((size_t)size);
  if (!data) {
    fclose(f);
    return NULL;
  }
  if (fseeko(f, (off_t)offset, SEEK_SET) != 0 ||
      fread(data, 1, (size_t)size, f) != (size_t)size) {
    debugPrintf("ep1_audio: read segment falhou off=%lld size=%lld\n", offset,
                size);
    free(data);
    data = NULL;
  }
  fclose(f);
  return data;
}

static int append_bytes(unsigned char **buf, size_t *len, size_t *cap,
                        const void *src, size_t n) {
  if (!n)
    return 1;
  size_t need = *len + n;
  if (need > *cap) {
    size_t nc = *cap ? *cap * 2 : 65536;
    while (nc < need)
      nc *= 2;
    unsigned char *p = realloc(*buf, nc);
    if (!p)
      return 0;
    *buf = p;
    *cap = nc;
  }
  memcpy(*buf + *len, src, n);
  *len += n;
  return 1;
}

static AudioBuffer *decode_mp3(const unsigned char *data, size_t size) {
  if (!g_mpg123_ready) {
    if (mpg123_init() != MPG123_OK)
      return NULL;
    g_mpg123_ready = 1;
  }

  int err = MPG123_OK;
  mpg123_handle *mh = mpg123_new(NULL, &err);
  if (!mh)
    return NULL;
  mpg123_format_none(mh);
  mpg123_format(mh, OUT_RATE, OUT_CH, MPG123_ENC_SIGNED_16);
  mpg123_param(mh, MPG123_FLAGS, MPG123_FORCE_STEREO, 0);
  if (mpg123_open_feed(mh) != MPG123_OK) {
    mpg123_delete(mh);
    return NULL;
  }
  mpg123_feed(mh, data, size);

  unsigned char tmp[32768];
  unsigned char *pcm = NULL;
  size_t pcm_len = 0, pcm_cap = 0;
  for (;;) {
    size_t done = 0;
    int ret = mpg123_read(mh, tmp, sizeof(tmp), &done);
    if (done && !append_bytes(&pcm, &pcm_len, &pcm_cap, tmp, done)) {
      free(pcm);
      pcm = NULL;
      pcm_len = 0;
      break;
    }
    if (ret == MPG123_OK || ret == MPG123_NEW_FORMAT)
      continue;
    if (ret == MPG123_NEED_MORE || ret == MPG123_DONE)
      break;
    debugPrintf("ep1_audio: mpg123_read ret=%d err=%s\n", ret,
                mpg123_strerror(mh));
    break;
  }
  mpg123_close(mh);
  mpg123_delete(mh);

  if (!pcm_len) {
    free(pcm);
    return NULL;
  }
  AudioBuffer *buf = calloc(1, sizeof(*buf));
  if (!buf) {
    free(pcm);
    return NULL;
  }
  buf->pcm = (int16_t *)pcm;
  buf->frames = (uint32_t)(pcm_len / (sizeof(int16_t) * OUT_CH));
  buf->duration_ms = (int)((uint64_t)buf->frames * 1000 / OUT_RATE);
  return buf;
}

static AudioCache *get_cached(const char *path, long long offset,
                              long long size) {
  for (int i = 0; i < MAX_AUDIO_CACHE; i++) {
    AudioCache *c = &g_cache[i];
    if (c->buf && c->offset == offset && c->size == size &&
        strcmp(c->path, path) == 0)
      return c;
  }

  int slot = -1;
  for (int i = 0; i < MAX_AUDIO_CACHE; i++) {
    if (!g_cache[i].buf) {
      slot = i;
      break;
    }
  }
  if (slot < 0)
    slot = 0;

  unsigned char *mp3 = read_segment(path, offset, size);
  if (!mp3)
    return NULL;
  AudioBuffer *buf = decode_mp3(mp3, (size_t)size);
  free(mp3);
  if (!buf)
    return NULL;

  AudioCache *c = &g_cache[slot];
  if (c->buf) {
    free(c->buf->pcm);
    free(c->buf);
  }
  memset(c, 0, sizeof(*c));
  snprintf(c->path, sizeof(c->path), "%s", path);
  c->offset = offset;
  c->size = size;
  c->buf = buf;
  resolve_zip_entry_name(path, offset, size, c->name, sizeof(c->name));
  debugPrintf("ep1_audio: decoded %s frames=%u ms=%d\n", c->name,
              c->buf->frames, c->buf->duration_ms);
  return c;
}

static int name_is_loop_music(const char *name) {
  if (!name)
    return 0;
  return strstr(name, "snd_sng_") != NULL;
}

static int name_is_gameplay_music(const char *name) {
  if (!name)
    return 0;
  return strstr(name, "snd_sng_z") || strstr(name, "snd_sng_boss") ||
         strstr(name, "snd_sng_special") || strstr(name, "snd_sng_final");
}

static int name_is_title_music(const char *name) {
  return name && strstr(name, "snd_sng_title") != NULL;
}

static int name_is_menu_music(const char *name) {
  return name && strstr(name, "snd_sng_menu") != NULL;
}

int ep1_audio_play_apk_mp3(const char *apk_path, int param, long long offset,
                           long long size, int channel) {
  (void)param;
  (void)channel;
  int handle = g_next_handle++;
  if (handle <= 0)
    handle = g_next_handle = 1;

  if (!ensure_audio())
    return handle;
  AudioCache *c = get_cached(apk_path, offset, size);
  if (!c || !c->buf)
    return handle;

  SDL_LockAudioDevice(g_audio_dev);
  AudioVoice *voice = NULL;
  for (int i = 0; i < MAX_AUDIO_VOICES; i++) {
    if (!g_voices[i].active) {
      voice = &g_voices[i];
      break;
    }
  }
  if (!voice)
    voice = &g_voices[0];
  memset(voice, 0, sizeof(*voice));
  voice->handle = handle;
  voice->buf = c->buf;
  voice->volume = 1.0f;
  voice->loop = name_is_loop_music(c->name);
  voice->active = 1;
  g_gameplay_music_active = name_is_gameplay_music(c->name);
  g_menu_music_active = name_is_menu_music(c->name);
  g_title_music_active = name_is_title_music(c->name);
  SDL_UnlockAudioDevice(g_audio_dev);

  debugPrintf("ep1_audio: play handle=%d loop=%d name=%s\n", handle,
              voice->loop, c->name);
  if (audio_log_enabled())
    debugPrintf("ep1_audio: args param=%d channel=%d off=%lld size=%lld\n",
                param, channel, offset, size);
  return handle;
}

void ep1_audio_stop(int handle) {
  if (!g_audio_ready)
    return;
  SDL_LockAudioDevice(g_audio_dev);
  for (int i = 0; i < MAX_AUDIO_VOICES; i++) {
    if (handle == 0 || g_voices[i].handle == handle)
      g_voices[i].active = 0;
  }
  if (handle == 0) {
    g_gameplay_music_active = 0;
    g_menu_music_active = 0;
    g_title_music_active = 0;
  }
  SDL_UnlockAudioDevice(g_audio_dev);
}

void ep1_audio_pause(int handle, int paused) {
  if (!g_audio_ready)
    return;
  SDL_LockAudioDevice(g_audio_dev);
  for (int i = 0; i < MAX_AUDIO_VOICES; i++) {
    if (handle == 0 || g_voices[i].handle == handle)
      g_voices[i].paused = paused;
  }
  SDL_UnlockAudioDevice(g_audio_dev);
}

void ep1_audio_set_volume(int a, int b) {
  if (!g_audio_ready)
    return;
  int handle = a;
  int vol = b;
  if (a > 16 && b <= 16) {
    vol = a;
    handle = b;
  }
  float f = (float)vol / 100.0f;
  if (f <= 0.0f)
    f = 1.0f;
  if (f > 1.5f)
    f = 1.5f;

  SDL_LockAudioDevice(g_audio_dev);
  for (int i = 0; i < MAX_AUDIO_VOICES; i++) {
    if (g_voices[i].active && (handle == 0 || g_voices[i].handle == handle))
      g_voices[i].volume = f;
  }
  SDL_UnlockAudioDevice(g_audio_dev);
}

int ep1_audio_is_playing(int handle) {
  if (!g_audio_ready)
    return 0;
  int playing = 0;
  SDL_LockAudioDevice(g_audio_dev);
  for (int i = 0; i < MAX_AUDIO_VOICES; i++) {
    if (g_voices[i].active && (handle == 0 || g_voices[i].handle == handle)) {
      playing = 1;
      break;
    }
  }
  SDL_UnlockAudioDevice(g_audio_dev);
  return playing;
}

int ep1_audio_status(int handle) { return ep1_audio_is_playing(handle) ? 1 : 0; }

int ep1_audio_duration_ms(int handle) {
  if (!g_audio_ready)
    return 0;
  int ret = 0;
  SDL_LockAudioDevice(g_audio_dev);
  for (int i = 0; i < MAX_AUDIO_VOICES; i++) {
    if (g_voices[i].active && (handle == 0 || g_voices[i].handle == handle) &&
        g_voices[i].buf) {
      ret = g_voices[i].buf->duration_ms;
      break;
    }
  }
  SDL_UnlockAudioDevice(g_audio_dev);
  return ret ? ret : 60000;
}

int ep1_audio_position_ms(int handle) {
  if (!g_audio_ready)
    return 0;
  int ret = 0;
  SDL_LockAudioDevice(g_audio_dev);
  for (int i = 0; i < MAX_AUDIO_VOICES; i++) {
    if (g_voices[i].active && (handle == 0 || g_voices[i].handle == handle)) {
      ret = (int)((uint64_t)g_voices[i].pos * 1000 / OUT_RATE);
      break;
    }
  }
  SDL_UnlockAudioDevice(g_audio_dev);
  return ret;
}

int ep1_audio_gameplay_music_active(void) { return g_gameplay_music_active; }

int ep1_audio_menu_music_active(void) { return g_menu_music_active; }

int ep1_audio_title_music_active(void) { return g_title_music_active; }

void ep1_audio_start_sfx(void *env, void *obj, ep1_generate_audio_fn generate,
                         ep1_make_short_array_fn make_array) {
  g_sfx_env = env;
  g_sfx_obj = obj;
  g_sfx_generate = generate;
  g_sfx_make_array = make_array;
  g_sfx_active = 1;
  ensure_audio();
  debugPrintf("ep1_audio: SFX generator started %p\n", (void *)generate);
}

void ep1_audio_stop_sfx(void) {
  g_sfx_active = 0;
  debugPrintf("ep1_audio: SFX generator stopped\n");
}
