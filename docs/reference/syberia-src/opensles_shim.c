/*
 * opensles_shim.c -- OpenSL ES to SDL2 audio bridge
 *
 * Translates OpenSL ES BufferQueue audio players into SDL2 audio output.
 * The game enqueues PCM buffers via SLBufferQueueItf; we copy them into
 * a lock-free SPSC ring buffer and feed them to SDL2's audio callback,
 * mixing all active players together.
 *
 * Threading model:
 *   - Game thread: calls bq_Enqueue (producer), fires callbacks
 *   - SDL audio thread: reads from ring buffer (consumer)
 *   - No mutexes on the audio path — fully lock-free SPSC
 */

#include <SDL2/SDL.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "opensles_shim.h"
#include "util.h"

/* ---- Constants ---- */

#define MAX_PLAYERS 16
#define RING_BUFFER_SIZE (256 * 1024) // 256KB ring buffer per player — power of 2
#define RING_BUFFER_MASK (RING_BUFFER_SIZE - 1)
#define SDL_AUDIO_SAMPLES 4096

/* ---- Interface ID storage ---- */

static const int id_engine_tag = 1;
static const int id_play_tag = 2;
static const int id_volume_tag = 3;
static const int id_bufferqueue_tag = 4;
static const int id_effectsend_tag = 5;

const SLInterfaceID sl_IID_ENGINE = &id_engine_tag;
const SLInterfaceID sl_IID_PLAY = &id_play_tag;
const SLInterfaceID sl_IID_VOLUME = &id_volume_tag;
const SLInterfaceID sl_IID_BUFFERQUEUE = &id_bufferqueue_tag;
const SLInterfaceID sl_IID_EFFECTSEND = &id_effectsend_tag;

/* ---- OpenSL ES data structures (from the spec) ---- */

typedef struct {
  SLuint32 locatorType;
  SLuint32 numBuffers;
} SLDataLocator_BufferQueue;

typedef struct {
  SLuint32 formatType;
  SLuint32 numChannels;
  SLuint32 samplesPerSec;   // milliHz, e.g. 44100000
  SLuint32 bitsPerSample;
  SLuint32 containerSize;
  SLuint32 channelMask;
  SLuint32 endianness;
} SLDataFormat_PCM;

typedef struct {
  void *pLocator;
  void *pFormat;
} SLDataSource;

typedef struct {
  void *pLocator;
  void *pFormat;
} SLDataSink;

/* ---- Per-player state ---- */

typedef void (*slBufferQueueCallback)(void *caller, void *pContext);

typedef struct {
  // Lock-free SPSC ring buffer (game thread writes, SDL thread reads)
  // Both indices only ever increase (masked on access). This avoids ABA.
  uint8_t ring[RING_BUFFER_SIZE];
  volatile uint32_t ring_head; // write position (game thread only)
  volatile uint32_t ring_tail; // read position (SDL thread only)

  // Callback (set once from game thread before playback starts)
  slBufferQueueCallback callback;
  void *callback_context;

  // How many bytes the game has enqueued since the last callback was fired.
  // Game thread only — used to decide when to fire the next callback.
  uint32_t enqueued_since_cb;
  uint32_t last_enqueue_size; // size of last Enqueue call (callback threshold)

  // Play callback for HEADATEND detection
  void (*play_callback)(void *caller, void *pContext, SLuint32 event);
  void *play_callback_context;
  SLuint32 play_event_mask;

  // Sound completion tracking (game thread only)
  uint32_t enqueue_counter;    // incremented on each bq_Enqueue call
  int ever_enqueued;           // set to 1 after first bq_Enqueue
  int headatend_fired;         // set to 1 after HEADATEND fired (prevent re-fire)
  int decoder_done;            // set to 1 when BQ callback fires but enqueues nothing

  // State
  volatile SLuint32 play_state;
  float volume; // linear 0.0 - 1.0
  int active;

  // Format
  SLuint32 num_channels;
  SLuint32 sample_rate; // Hz
  SLuint32 bits_per_sample;

  // Vtables (each player has its own vtable pointers)
  void *obj_vtable[8];
  void *obj_ptr;

  void *play_vtable[8];
  void *play_ptr;

  void *volume_vtable[8];
  void *volume_ptr;

  void *bq_vtable[8];
  void *bq_ptr;

  void *effectsend_vtable[8];
  void *effectsend_ptr;
} AudioPlayer;

/* ---- Globals ---- */

static AudioPlayer g_players[MAX_PLAYERS];
static pthread_mutex_t g_players_lock = PTHREAD_MUTEX_INITIALIZER;
static SDL_AudioDeviceID g_audio_dev = 0;
static int g_audio_initialized = 0;

/* ---- Lock-free SPSC ring buffer helpers ---- */

// How many bytes are available to read (SDL thread perspective)
static inline uint32_t ring_readable(const AudioPlayer *p) {
  return p->ring_head - p->ring_tail;
}

// How many bytes of free space for writing (game thread perspective)
static inline uint32_t ring_writable(const AudioPlayer *p) {
  return RING_BUFFER_SIZE - (p->ring_head - p->ring_tail);
}

// Write data into ring buffer (game thread only)
static uint32_t ring_write(AudioPlayer *p, const void *data, uint32_t len) {
  uint32_t space = ring_writable(p);
  if (len > space)
    len = space;
  if (len == 0)
    return 0;

  const uint8_t *src = (const uint8_t *)data;
  uint32_t head = p->ring_head & RING_BUFFER_MASK;
  uint32_t first = RING_BUFFER_SIZE - head;
  if (first > len)
    first = len;
  memcpy(p->ring + head, src, first);
  if (len > first)
    memcpy(p->ring, src + first, len - first);

  // Memory barrier: ensure data is written before head advances
  __sync_synchronize();
  p->ring_head += len;
  return len;
}

// Read data from ring buffer (SDL thread only)
static uint32_t ring_read(AudioPlayer *p, void *data, uint32_t len) {
  uint32_t avail = ring_readable(p);
  if (len > avail)
    len = avail;
  if (len == 0)
    return 0;

  uint8_t *dst = (uint8_t *)data;
  uint32_t tail = p->ring_tail & RING_BUFFER_MASK;
  uint32_t first = RING_BUFFER_SIZE - tail;
  if (first > len)
    first = len;
  memcpy(dst, p->ring + tail, first);
  if (len > first)
    memcpy(dst + first, p->ring, len - first);

  // Memory barrier: ensure data is read before tail advances
  __sync_synchronize();
  p->ring_tail += len;
  return len;
}

/* ---- SDL2 audio callback (runs on SDL audio thread) ---- */

#define SDL_OUTPUT_RATE 44100

// Temporary buffer large enough for resampling: worst case is reading
// at a lower rate (e.g. 22050→44100 needs half the output frames from source)
// but we need space for the full output in the tmp buffer.
static int16_t g_tmp[SDL_AUDIO_SAMPLES * 2];

static void sdl_audio_callback(void *userdata, Uint8 *stream, int len) {
  (void)userdata;
  memset(stream, 0, len);

  int16_t *out = (int16_t *)stream;
  int out_samples = len / sizeof(int16_t);

  static int32_t mix_buf[SDL_AUDIO_SAMPLES * 2];
  if (out_samples > SDL_AUDIO_SAMPLES * 2)
    out_samples = SDL_AUDIO_SAMPLES * 2;
  memset(mix_buf, 0, out_samples * sizeof(int32_t));

  // Output is always stereo 44100 Hz
  uint32_t out_frames = out_samples / 2;

  for (int i = 0; i < MAX_PLAYERS; i++) {
    AudioPlayer *p = &g_players[i];
    if (!p->active || p->play_state != SL_PLAYSTATE_PLAYING)
      continue;

    uint32_t src_rate = p->sample_rate;
    if (src_rate == 0)
      src_rate = SDL_OUTPUT_RATE;

    uint32_t src_channels = p->num_channels;
    if (src_channels == 0)
      src_channels = 2;

    uint32_t frame_size = src_channels * sizeof(int16_t);
    float vol = p->volume;

    // How many source frames do we need to produce out_frames output frames?
    uint32_t src_frames_needed;
    if (src_rate == SDL_OUTPUT_RATE) {
      src_frames_needed = out_frames;
    } else {
      // For resampling: we need (out_frames * src_rate / output_rate) source frames
      src_frames_needed = (uint32_t)((uint64_t)out_frames * src_rate / SDL_OUTPUT_RATE) + 1;
    }

    // Read source frames from ring buffer
    uint32_t src_bytes_want = src_frames_needed * frame_size;
    if (src_bytes_want > sizeof(g_tmp))
      src_bytes_want = sizeof(g_tmp);
    src_bytes_want = (src_bytes_want / frame_size) * frame_size;

    uint32_t got = ring_read(p, g_tmp, src_bytes_want);
    got = (got / frame_size) * frame_size;
    uint32_t src_frames_got = got / frame_size;

    if (src_frames_got == 0)
      continue;

    // Resample + mix into output
    if (src_rate == SDL_OUTPUT_RATE && src_channels == 2) {
      // Fast path: no resampling needed, stereo → stereo
      uint32_t n = src_frames_got;
      if (n > out_frames)
        n = out_frames;
      for (uint32_t f = 0; f < n; f++) {
        mix_buf[f * 2]     += (int32_t)(g_tmp[f * 2]     * vol);
        mix_buf[f * 2 + 1] += (int32_t)(g_tmp[f * 2 + 1] * vol);
      }
      // Fade out on underrun
      if (n > 0 && n < out_frames) {
        uint32_t fade_len = 64;
        if (fade_len > n) fade_len = n;
        uint32_t fade_start = n - fade_len;
        for (uint32_t s = 0; s < fade_len; s++) {
          float fade = 1.0f - (float)(s + 1) / (float)fade_len;
          uint32_t idx = (fade_start + s) * 2;
          mix_buf[idx]     = (int32_t)(mix_buf[idx]     * fade);
          mix_buf[idx + 1] = (int32_t)(mix_buf[idx + 1] * fade);
        }
      }
    } else {
      // Resample with linear interpolation
      // Source position in fixed-point (16.16)
      uint32_t step = (uint32_t)((uint64_t)src_rate * 65536 / SDL_OUTPUT_RATE);
      uint32_t pos = 0; // 16.16 fixed point

      for (uint32_t f = 0; f < out_frames; f++) {
        uint32_t idx = pos >> 16;
        uint32_t frac = pos & 0xFFFF;

        if (idx >= src_frames_got)
          break;

        int32_t l0, r0, l1, r1;
        if (src_channels == 1) {
          l0 = g_tmp[idx];
          r0 = l0;
          l1 = (idx + 1 < src_frames_got) ? g_tmp[idx + 1] : l0;
          r1 = l1;
        } else {
          l0 = g_tmp[idx * 2];
          r0 = g_tmp[idx * 2 + 1];
          if (idx + 1 < src_frames_got) {
            l1 = g_tmp[(idx + 1) * 2];
            r1 = g_tmp[(idx + 1) * 2 + 1];
          } else {
            l1 = l0;
            r1 = r0;
          }
        }

        // Linear interpolation
        int32_t left  = l0 + (int32_t)(((int64_t)(l1 - l0) * frac) >> 16);
        int32_t right = r0 + (int32_t)(((int64_t)(r1 - r0) * frac) >> 16);

        mix_buf[f * 2]     += (int32_t)(left * vol);
        mix_buf[f * 2 + 1] += (int32_t)(right * vol);

        pos += step;
      }
    }
  }

  // Clamp and write to output
  for (int s = 0; s < out_samples; s++) {
    int32_t v = mix_buf[s];
    if (v > 32767)
      v = 32767;
    if (v < -32768)
      v = -32768;
    out[s] = (int16_t)v;
  }
}

/* ---- Initialize SDL2 audio ---- */

static void ensure_audio_initialized(void) {
  if (g_audio_initialized)
    return;

  SDL_AudioSpec want, have;
  memset(&want, 0, sizeof(want));
  want.freq = 44100;
  want.format = AUDIO_S16SYS;
  want.channels = 2;
  want.samples = SDL_AUDIO_SAMPLES;
  want.callback = sdl_audio_callback;
  want.userdata = NULL;

  g_audio_dev =
      SDL_OpenAudioDevice(NULL, 0, &want, &have, SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
  if (g_audio_dev == 0) {
    debugPrintf("opensles_shim: SDL_OpenAudioDevice failed: %s\n",
                SDL_GetError());
    g_audio_initialized = 1;
    return;
  }

  debugPrintf("opensles_shim: SDL audio opened: %dHz %dch %d samples\n",
              have.freq, have.channels, have.samples);

  SDL_PauseAudioDevice(g_audio_dev, 0);
  g_audio_initialized = 1;
}

/* ---- Allocate a player ---- */

static AudioPlayer *alloc_player(void) {
  pthread_mutex_lock(&g_players_lock);
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (!g_players[i].active) {
      AudioPlayer *p = &g_players[i];
      memset(p, 0, sizeof(*p));
      p->active = 1;
      p->volume = 1.0f;
      p->play_state = SL_PLAYSTATE_STOPPED;
      pthread_mutex_unlock(&g_players_lock);
      debugPrintf("opensles_shim: allocated player %d\n", i);
      return p;
    }
  }
  pthread_mutex_unlock(&g_players_lock);
  debugPrintf("opensles_shim: WARNING: no free player slots!\n");
  return NULL;
}

/* ---- SLPlayItf methods ---- */

static SLresult play_SetPlayState(void *self, SLuint32 state) {
  void **itf_ptr = (void **)self;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (&g_players[i].play_ptr == itf_ptr) {
      AudioPlayer *p = &g_players[i];
      if (state == SL_PLAYSTATE_PLAYING && p->play_state != SL_PLAYSTATE_PLAYING) {
        p->enqueue_counter = 0;
        p->ever_enqueued = 0;
        p->headatend_fired = 0;
        p->decoder_done = 0;
      }
      p->play_state = state;
      return SL_RESULT_SUCCESS;
    }
  }
  return SL_RESULT_SUCCESS;
}

static SLresult play_GetPlayState(void *self, SLuint32 *pState) {
  void **itf_ptr = (void **)self;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (&g_players[i].play_ptr == itf_ptr) {
      if (pState)
        *pState = g_players[i].play_state;
      return SL_RESULT_SUCCESS;
    }
  }
  if (pState)
    *pState = SL_PLAYSTATE_STOPPED;
  return SL_RESULT_SUCCESS;
}

static SLresult play_RegisterCallback(void *self, void *callback, void *ctx) {
  void **itf_ptr = (void **)self;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (&g_players[i].play_ptr == itf_ptr) {
      g_players[i].play_callback =
          (void (*)(void *, void *, SLuint32))callback;
      g_players[i].play_callback_context = ctx;
      debugPrintf("opensles_shim: player %d registered play callback %p\n",
                  i, callback);
      return SL_RESULT_SUCCESS;
    }
  }
  return SL_RESULT_SUCCESS;
}

static SLresult play_SetCallbackEventsMask(void *self, SLuint32 eventFlags) {
  void **itf_ptr = (void **)self;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (&g_players[i].play_ptr == itf_ptr) {
      g_players[i].play_event_mask = eventFlags;
      return SL_RESULT_SUCCESS;
    }
  }
  return SL_RESULT_SUCCESS;
}

/* ---- SLVolumeItf methods ---- */

static SLresult volume_SetVolumeLevel(void *self, SLmillibel level) {
  float linear;
  if (level <= -9600)
    linear = 0.0f;
  else
    linear = powf(10.0f, level / 2000.0f);

  void **itf_ptr = (void **)self;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (&g_players[i].volume_ptr == itf_ptr) {
      g_players[i].volume = linear;
      return SL_RESULT_SUCCESS;
    }
  }
  return SL_RESULT_SUCCESS;
}

static SLresult volume_GetVolumeLevel(void *self, SLmillibel *pLevel) {
  (void)self;
  if (pLevel)
    *pLevel = 0;
  return SL_RESULT_SUCCESS;
}

static SLresult volume_GetMaxVolumeLevel(void *self, SLmillibel *pMaxLevel) {
  (void)self;
  if (pMaxLevel)
    *pMaxLevel = 0;
  return SL_RESULT_SUCCESS;
}

/* ---- SLBufferQueueItf methods ---- */

static SLresult bq_Enqueue(void *self, const void *pBuffer, SLuint32 size) {
  void **itf_ptr = (void **)self;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (&g_players[i].bq_ptr == itf_ptr) {
      AudioPlayer *p = &g_players[i];

      uint32_t written = ring_write(p, pBuffer, size);
      if (written < size) {
        debugPrintf("opensles_shim: player %d ring overflow "
                    "(wanted %u, wrote %u)\n",
                    i, size, written);
      }

      // Track how much we've enqueued; remember the buffer size for
      // threshold-based callback firing
      p->enqueued_since_cb += written;
      if (size > 0)
        p->last_enqueue_size = size;

      // Track for HEADATEND detection
      p->enqueue_counter++;
      p->ever_enqueued = 1;

      return SL_RESULT_SUCCESS;
    }
  }
  return SL_RESULT_SUCCESS;
}

static SLresult bq_Clear(void *self) {
  void **itf_ptr = (void **)self;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (&g_players[i].bq_ptr == itf_ptr) {
      AudioPlayer *p = &g_players[i];
      p->ring_head = 0;
      p->ring_tail = 0;
      p->enqueued_since_cb = 0;
      p->enqueue_counter = 0;
      p->ever_enqueued = 0;
      p->headatend_fired = 0;
      p->decoder_done = 0;
      return SL_RESULT_SUCCESS;
    }
  }
  return SL_RESULT_SUCCESS;
}

static SLresult bq_GetState(void *self, void *pState) {
  (void)self;
  // SLBufferQueueState { SLuint32 count; SLuint32 playIndex; }
  if (pState) {
    SLuint32 *state = (SLuint32 *)pState;
    state[0] = 0; // count
    state[1] = 0; // playIndex
  }
  return SL_RESULT_SUCCESS;
}

static SLresult bq_RegisterCallback(void *self, slBufferQueueCallback callback,
                                     void *pContext) {
  void **itf_ptr = (void **)self;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (&g_players[i].bq_ptr == itf_ptr) {
      AudioPlayer *p = &g_players[i];
      p->callback = callback;
      p->callback_context = pContext;
      debugPrintf("opensles_shim: player %d registered BQ callback %p\n", i,
                  (void *)callback);
      return SL_RESULT_SUCCESS;
    }
  }
  return SL_RESULT_SUCCESS;
}

/* ---- Stub for unused interfaces ---- */

static SLresult stub_success(void) { return SL_RESULT_SUCCESS; }

/* ---- Player object methods ---- */

static SLresult player_Realize(void *self, SLBoolean async) {
  (void)self;
  (void)async;
  return SL_RESULT_SUCCESS;
}

static SLresult player_GetInterface(void *self, SLInterfaceID iid,
                                     void **pInterface) {
  void **obj_ptr = (void **)self;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (&g_players[i].obj_ptr == obj_ptr) {
      AudioPlayer *p = &g_players[i];
      if (iid == sl_IID_PLAY) {
        *pInterface = &p->play_ptr;
      } else if (iid == sl_IID_VOLUME) {
        *pInterface = &p->volume_ptr;
      } else if (iid == sl_IID_BUFFERQUEUE) {
        *pInterface = &p->bq_ptr;
      } else if (iid == sl_IID_EFFECTSEND) {
        *pInterface = &p->effectsend_ptr;
      } else {
        *pInterface = &p->effectsend_ptr;
      }
      return SL_RESULT_SUCCESS;
    }
  }
  return SL_RESULT_SUCCESS;
}

static void player_Destroy(void *self) {
  void **obj_ptr = (void **)self;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (&g_players[i].obj_ptr == obj_ptr) {
      AudioPlayer *p = &g_players[i];
      debugPrintf("opensles_shim: destroying player %d\n", i);
      p->play_state = SL_PLAYSTATE_STOPPED;
      __sync_synchronize();
      p->active = 0;
      return;
    }
  }
}

/* ---- Setup player vtables ---- */

static void setup_player_vtables(AudioPlayer *p) {
  // Object vtable: Realize=0, Resume=1, GetState=2, GetInterface=3,
  //                RegisterCallback=4, AbortAsyncOperation=5, Destroy=6
  for (int i = 0; i < 8; i++)
    p->obj_vtable[i] = (void *)stub_success;
  p->obj_vtable[0] = (void *)player_Realize;
  p->obj_vtable[3] = (void *)player_GetInterface;
  p->obj_vtable[6] = (void *)player_Destroy;
  p->obj_ptr = p->obj_vtable;

  // Play vtable: SetPlayState=0, GetPlayState=1, GetDuration=2,
  //              GetPosition=3, RegisterCallback=4,
  //              SetCallbackEventsMask=5, GetCallbackEventsMask=6
  for (int i = 0; i < 8; i++)
    p->play_vtable[i] = (void *)stub_success;
  p->play_vtable[0] = (void *)play_SetPlayState;
  p->play_vtable[1] = (void *)play_GetPlayState;
  p->play_vtable[4] = (void *)play_RegisterCallback;
  p->play_vtable[5] = (void *)play_SetCallbackEventsMask;
  p->play_ptr = p->play_vtable;

  // Volume vtable: SetVolumeLevel=0, GetVolumeLevel=1, GetMaxVolumeLevel=2
  for (int i = 0; i < 8; i++)
    p->volume_vtable[i] = (void *)stub_success;
  p->volume_vtable[0] = (void *)volume_SetVolumeLevel;
  p->volume_vtable[1] = (void *)volume_GetVolumeLevel;
  p->volume_vtable[2] = (void *)volume_GetMaxVolumeLevel;
  p->volume_ptr = p->volume_vtable;

  // BufferQueue vtable: Enqueue=0, Clear=1, GetState=2, RegisterCallback=3
  for (int i = 0; i < 8; i++)
    p->bq_vtable[i] = (void *)stub_success;
  p->bq_vtable[0] = (void *)bq_Enqueue;
  p->bq_vtable[1] = (void *)bq_Clear;
  p->bq_vtable[2] = (void *)bq_GetState;
  p->bq_vtable[3] = (void *)bq_RegisterCallback;
  p->bq_ptr = p->bq_vtable;

  // EffectSend vtable: all stubs
  for (int i = 0; i < 8; i++)
    p->effectsend_vtable[i] = (void *)stub_success;
  p->effectsend_ptr = p->effectsend_vtable;
}

/* ---- Output mix (mostly stub) ---- */

static void *g_outmix_vtable[8];
static void *g_outmix_ptr;

static SLresult outmix_Realize(void *self, SLBoolean async) {
  (void)self;
  (void)async;
  return SL_RESULT_SUCCESS;
}

static SLresult outmix_GetInterface(void *self, SLInterfaceID iid,
                                     void **pInterface) {
  (void)self;
  (void)iid;
  static void *stub_vtable[8];
  static void *stub_ptr;
  static int inited = 0;
  if (!inited) {
    for (int i = 0; i < 8; i++)
      stub_vtable[i] = (void *)stub_success;
    stub_ptr = stub_vtable;
    inited = 1;
  }
  if (pInterface)
    *pInterface = &stub_ptr;
  return SL_RESULT_SUCCESS;
}

static void outmix_Destroy(void *self) { (void)self; }

static void init_outmix(void) {
  static int inited = 0;
  if (inited)
    return;
  inited = 1;
  for (int i = 0; i < 8; i++)
    g_outmix_vtable[i] = (void *)stub_success;
  g_outmix_vtable[0] = (void *)outmix_Realize;
  g_outmix_vtable[3] = (void *)outmix_GetInterface;
  g_outmix_vtable[6] = (void *)outmix_Destroy;
  g_outmix_ptr = g_outmix_vtable;
}

/* ---- Engine interface: CreateOutputMix, CreateAudioPlayer ---- */

static SLresult engine_CreateOutputMix(void *self, void **pMix,
                                        SLuint32 numInterfaces,
                                        const SLInterfaceID *pInterfaceIds,
                                        const SLBoolean *pInterfaceRequired) {
  (void)self;
  (void)numInterfaces;
  (void)pInterfaceIds;
  (void)pInterfaceRequired;

  debugPrintf("opensles_shim: CreateOutputMix\n");
  init_outmix();
  if (pMix)
    *pMix = &g_outmix_ptr;
  return SL_RESULT_SUCCESS;
}

static SLresult engine_CreateAudioPlayer(void *self, void **pPlayer,
                                          void *pAudioSrc, void *pAudioSnk,
                                          SLuint32 numInterfaces,
                                          const SLInterfaceID *pInterfaceIds,
                                          const SLBoolean *pInterfaceRequired) {
  (void)self;
  (void)pAudioSnk;
  (void)numInterfaces;
  (void)pInterfaceIds;
  (void)pInterfaceRequired;

  debugPrintf("opensles_shim: CreateAudioPlayer\n");

  ensure_audio_initialized();

  AudioPlayer *p = alloc_player();
  if (!p) {
    p = &g_players[0];
  }

  // Parse audio format from pAudioSrc
  if (pAudioSrc) {
    SLDataSource *src = (SLDataSource *)pAudioSrc;
    debugPrintf("opensles_shim: pAudioSrc=%p pLocator=%p pFormat=%p\n",
                pAudioSrc, src->pLocator, src->pFormat);
    if (src->pFormat) {
      SLDataFormat_PCM *fmt = (SLDataFormat_PCM *)src->pFormat;
      if (fmt->formatType == SL_DATAFORMAT_PCM) {
        p->num_channels = fmt->numChannels;
        p->sample_rate = fmt->samplesPerSec / 1000; // milliHz -> Hz
        p->bits_per_sample = fmt->bitsPerSample;
        debugPrintf("opensles_shim: format: %u ch, %u Hz, %u bit\n",
                    p->num_channels, p->sample_rate, p->bits_per_sample);
      }
    }
  }

  // Default format if not specified
  if (p->sample_rate == 0) {
    p->num_channels = 2;
    p->sample_rate = 44100;
    p->bits_per_sample = 16;
    debugPrintf("opensles_shim: using default format: 2ch 44100Hz 16bit\n");
  }

  setup_player_vtables(p);

  if (pPlayer)
    *pPlayer = &p->obj_ptr;
  return SL_RESULT_SUCCESS;
}

/* ---- Engine object ---- */

static void *g_engine_obj_vtable[8];
static void *g_engine_obj_ptr;

static void *g_engine_itf_vtable[16];
static void *g_engine_itf_ptr;

static SLresult engine_obj_Realize(void *self, SLBoolean async) {
  (void)self;
  (void)async;
  return SL_RESULT_SUCCESS;
}

static SLresult engine_obj_GetInterface(void *self, SLInterfaceID iid,
                                         void **pInterface) {
  (void)self;
  if (iid == sl_IID_ENGINE) {
    if (pInterface)
      *pInterface = &g_engine_itf_ptr;
  } else {
    static void *stub_vtable[8];
    static void *stub_ptr;
    static int inited = 0;
    if (!inited) {
      for (int i = 0; i < 8; i++)
        stub_vtable[i] = (void *)stub_success;
      stub_ptr = stub_vtable;
      inited = 1;
    }
    if (pInterface)
      *pInterface = &stub_ptr;
  }
  return SL_RESULT_SUCCESS;
}

static void engine_obj_Destroy(void *self) {
  (void)self;
  debugPrintf("opensles_shim: Engine Destroy\n");
  if (g_audio_dev) {
    SDL_CloseAudioDevice(g_audio_dev);
    g_audio_dev = 0;
    g_audio_initialized = 0;
  }
}

static void init_engine(void) {
  static int inited = 0;
  if (inited)
    return;
  inited = 1;

  for (int i = 0; i < 8; i++)
    g_engine_obj_vtable[i] = (void *)stub_success;
  g_engine_obj_vtable[0] = (void *)engine_obj_Realize;
  g_engine_obj_vtable[3] = (void *)engine_obj_GetInterface;
  g_engine_obj_vtable[6] = (void *)engine_obj_Destroy;
  g_engine_obj_ptr = g_engine_obj_vtable;

  for (int i = 0; i < 16; i++)
    g_engine_itf_vtable[i] = (void *)stub_success;
  g_engine_itf_vtable[2] = (void *)engine_CreateAudioPlayer;
  g_engine_itf_vtable[7] = (void *)engine_CreateOutputMix;
  g_engine_itf_ptr = g_engine_itf_vtable;
}

/* ---- Public API ---- */

void opensles_shim_pump_callbacks(void) {
  // Called from game thread (ALooper_pollAll).
  // Two jobs:
  //   1. Fire BQ callbacks to request more decoded audio data
  //   2. Detect when a sound has finished (HEADATEND)
  for (int i = 0; i < MAX_PLAYERS; i++) {
    AudioPlayer *p = &g_players[i];
    if (!p->active)
      continue;
    if (p->play_state != SL_PLAYSTATE_PLAYING)
      continue;

    uint32_t readable = ring_readable(p);

    // --- Phase 1: Feed the decoder ---
    if (p->callback && readable < RING_BUFFER_SIZE / 2) {
      uint32_t counter_before = p->enqueue_counter;

      p->callback(&p->bq_ptr, p->callback_context);

      // If BQ callback didn't enqueue anything, the decoder is done
      if (p->ever_enqueued && !p->decoder_done &&
          p->enqueue_counter == counter_before) {
        p->decoder_done = 1;
        debugPrintf("opensles_shim: player %d decoder_done (no data after BQ cb, "
                    "ring=%u)\n", i, ring_readable(p));
      }
    }

    // --- Phase 2: HEADATEND detection ---
    // Once decoder is done and ring buffer has fully drained, signal completion.
    // Instead of calling isPlayingCallback (which locks TeMutex from the game
    // thread and can deadlock), we write the stopRequested flag directly.
    // On Android, isPlayingCallback runs on the audio thread and does:
    //   TeMutex::lock(temusic + 0x88);
    //   repeat = *(temusic + 0x218);
    //   TeMutex::unlock(temusic + 0x88);
    //   if (!repeat) *(temusic + 0x1c0) = 1;  // stopRequested
    // We skip the mutex (same thread) and write the flag directly.
    if (p->decoder_done && !p->headatend_fired) {
      uint32_t readable_now = ring_readable(p);
      if (readable_now == 0) {
        p->headatend_fired = 1;
        if (p->play_callback_context) {
          char *temusic = (char *)p->play_callback_context;
          char repeat_flag = *(temusic + 0x218);
          if (!repeat_flag) {
            *(temusic + 0x1c0) = 1; // stopRequested
            debugPrintf("opensles_shim: player %d HEADATEND — set stopRequested "
                        "(ctx=%p)\n", i, p->play_callback_context);
          } else {
            debugPrintf("opensles_shim: player %d sound done, repeat=1 skipping\n", i);
          }
        }
      }
    }
  }
}

SLresult slCreateEngine_shim(void **pEngine, SLuint32 numOptions,
                              const void *pEngineOptions,
                              SLuint32 numInterfaces,
                              const SLInterfaceID *pInterfaceIds,
                              const SLBoolean *pInterfaceRequired) {
  (void)numOptions;
  (void)pEngineOptions;
  (void)numInterfaces;
  (void)pInterfaceIds;
  (void)pInterfaceRequired;

  debugPrintf("opensles_shim: slCreateEngine\n");

  init_engine();

  if (pEngine)
    *pEngine = &g_engine_obj_ptr;

  return SL_RESULT_SUCCESS;
}
