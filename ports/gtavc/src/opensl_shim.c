/* opensl_shim.c -- OpenSL ES (Android) -> SDL_Audio bridge.
 *
 * O GTA SA traz OpenAL-Soft ESTÁTICO cujo backend de saída no Android é OpenSL ES
 * (OSWrapper/OAL/Soft/opensl.c). O device (Amlogic AML-M8AUDIO I2S) não tem OpenSL,
 * mas tem SDL2/ALSA. Aqui implementamos a superfície OpenSL ES 1.0.1 que o
 * opensl_open/reset/start_playback usa e mandamos o PCM pro SDL_Audio.
 *
 * Fluxo OpenAL-Soft (opensl.c):
 *   slCreateEngine -> Realize -> GetInterface(SL_IID_ENGINE)
 *   CreateOutputMix -> Realize
 *   CreateAudioPlayer(src=SimpleBufferQueue+PCM, snk=OutputMix) -> Realize
 *   GetInterface(SL_IID_PLAY) ; GetInterface(SL_IID_ANDROIDSIMPLEBUFFERQUEUE)
 *   RegisterCallback(bq, cb, ctx) ; SetPlayState(PLAYING) ; Enqueue(...) xN
 *   cb(bq, ctx) dispara quando um buffer termina -> OpenAL-Soft enfileira o próximo.
 *
 * Gate: só ativo com GTASA_AUDIO=1 (o build estável default é silencioso).
 */
#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* ---- tipos OpenSL ES mínimos ---- */
typedef uint32_t SLuint32;
typedef int32_t  SLresult;
typedef uint32_t SLboolean;
#define SL_RESULT_SUCCESS 0
#define SL_BOOLEAN_FALSE  0
#define SL_PLAYSTATE_PLAYING 3

typedef const void **SLObjectItf;   /* ponteiro p/ ponteiro-de-vtable */
typedef const void **SLEngineItf;
typedef const void **SLPlayItf;
typedef const void **SLBufQueueItf;

typedef struct { SLuint32 locatorType; SLuint32 numBuffers; } SLDataLocator_BQ;
typedef struct { SLuint32 formatType; SLuint32 numChannels; SLuint32 samplesPerSec;
                 SLuint32 bitsPerSample; SLuint32 containerSize; SLuint32 channelMask;
                 SLuint32 endianness; } SLDataFormat_PCM;
typedef struct { void *pLocator; void *pFormat; } SLDataSource, SLDataSink;

typedef void (*slBQCallback)(SLBufQueueItf caller, void *pContext);

/* ---- estado do player (único; GTA SA abre 1 device de saída) ---- */
/* Cada "interface" OpenSL = ponteiro p/ um campo que guarda o ptr-de-vtable.
 * Campo = `const void*` (guarda a vtable-array como void*); a interface passada
 * aos métodos é `&campo` (const void**). (*itf) = vtable; (*itf)[i] = método. */
#define MAXQ 8
typedef struct { const void *buf; SLuint32 size, pos; } QBuf;
typedef struct {
  const void *objItf;    /* -> obj_vtbl */
  const void *playItf;   /* -> play_vtbl */
  const void *bqItf;     /* -> bq_vtbl */
  slBQCallback cb; void *cbctx;
  QBuf q[MAXQ]; int qhead, qcount;
  pthread_mutex_t lock;
  SDL_AudioDeviceID dev;
  int channels, rate, bits, playing;
} Player;

typedef struct { const void *objItf; const void *engItf; } Engine;
typedef struct { const void *objItf; } OutputMix;

static Player g_player;  /* GTA SA usa 1 player de saída */

/* ---- SDL audio callback: dreno da fila de buffers enfileirados ---- */
static void sdl_audio_cb(void *ud, Uint8 *stream, int len) {
  Player *p = (Player *)ud;
  int done = 0;
  pthread_mutex_lock(&p->lock);
  while (done < len && p->qcount > 0) {
    QBuf *b = &p->q[p->qhead];
    int avail = (int)b->size - (int)b->pos;
    int n = (len - done < avail) ? (len - done) : avail;
    memcpy(stream + done, (const uint8_t *)b->buf + b->pos, n);
    b->pos += n; done += n;
    if (b->pos >= b->size) {              /* buffer consumido -> avisa OpenAL-Soft */
      p->qhead = (p->qhead + 1) % MAXQ; p->qcount--;
      slBQCallback cb = p->cb; void *ctx = p->cbctx;
      pthread_mutex_unlock(&p->lock);
      if (cb) cb(&p->bqItf, ctx);          /* OpenAL-Soft enfileira o próximo */
      pthread_mutex_lock(&p->lock);
    }
  }
  pthread_mutex_unlock(&p->lock);
  if (done < len) memset(stream + done, 0, len - done);  /* underrun -> silêncio */
}

/* ---- SLAndroidSimpleBufferQueueItf ---- */
static SLresult bq_Enqueue(SLBufQueueItf self, const void *pBuffer, SLuint32 size) {
  Player *p = (Player *)((char *)self - offsetof(Player, bqItf));
  pthread_mutex_lock(&p->lock);
  if (p->qcount < MAXQ) {
    int t = (p->qhead + p->qcount) % MAXQ;
    p->q[t].buf = pBuffer; p->q[t].size = size; p->q[t].pos = 0; p->qcount++;
  }
  pthread_mutex_unlock(&p->lock);
  return SL_RESULT_SUCCESS;
}
static SLresult bq_Clear(SLBufQueueItf self) {
  Player *p = (Player *)((char *)self - offsetof(Player, bqItf));
  pthread_mutex_lock(&p->lock); p->qhead = p->qcount = 0; pthread_mutex_unlock(&p->lock);
  return SL_RESULT_SUCCESS;
}
static SLresult bq_GetState(SLBufQueueItf self, void *pState) {
  Player *p = (Player *)((char *)self - offsetof(Player, bqItf));
  if (pState) { ((SLuint32 *)pState)[0] = p->qcount; ((SLuint32 *)pState)[1] = 0; }
  return SL_RESULT_SUCCESS;
}
static SLresult bq_RegisterCallback(SLBufQueueItf self, slBQCallback cb, void *ctx) {
  Player *p = (Player *)((char *)self - offsetof(Player, bqItf));
  p->cb = cb; p->cbctx = ctx; return SL_RESULT_SUCCESS;
}
static const void *bq_vtbl[] = { bq_Enqueue, bq_Clear, bq_GetState, bq_RegisterCallback };

/* ---- SLPlayItf ---- */
static SLresult play_SetPlayState(SLPlayItf self, SLuint32 state) {
  Player *p = (Player *)((char *)self - offsetof(Player, playItf));
  if (state == SL_PLAYSTATE_PLAYING && p->dev) { p->playing = 1; SDL_PauseAudioDevice(p->dev, 0); }
  else if (p->dev) { p->playing = 0; SDL_PauseAudioDevice(p->dev, 1); }
  return SL_RESULT_SUCCESS;
}
static SLresult play_ret0(void) { return SL_RESULT_SUCCESS; }
static const void *play_vtbl[] = {
  play_SetPlayState, play_ret0, play_ret0, play_ret0, play_ret0, play_ret0,
  play_ret0, play_ret0, play_ret0, play_ret0, play_ret0, play_ret0,
};

/* ---- SLObjectItf (comum a engine/outputmix/player) ---- */
static SLresult obj_Realize(SLObjectItf self, SLboolean async) { (void)self; (void)async; return SL_RESULT_SUCCESS; }
static SLresult obj_GetState(SLObjectItf self, void *pState) { (void)self; if (pState) *(SLuint32 *)pState = 2 /*REALIZED*/; return SL_RESULT_SUCCESS; }
/* SL_IID_* = ponteiros distintos (definidos abaixo); GetInterface casa por ponteiro */
extern const void *const gtasa_SL_IID_ENGINE;
extern const void *const gtasa_SL_IID_PLAY;
extern const void *const gtasa_SL_IID_BUFFERQUEUE;
extern const void *const gtasa_SL_IID_ANDROIDSIMPLEBUFFERQUEUE;
/* casa o IID tanto pelo VALOR (jogo derefencia a variável SL_IID_*) quanto pelo
 * ENDEREÇO (caso passe &var direto) — robusto aos dois modos de acesso. */
#define IID_IS(iid, g) ((iid) == (g) || (iid) == (const void *)&(g))
static SLresult obj_GetInterface(SLObjectItf self, const void *iid, void *pItf) {
  /* self aponta p/ o 1o campo (objItf) do Engine/Player/OutputMix */
  if (IID_IS(iid, gtasa_SL_IID_ENGINE)) {
    Engine *e = (Engine *)self; *(void **)pItf = &e->engItf; return SL_RESULT_SUCCESS;
  }
  if (IID_IS(iid, gtasa_SL_IID_PLAY)) {
    Player *p = (Player *)self; *(void **)pItf = &p->playItf; return SL_RESULT_SUCCESS;
  }
  if (IID_IS(iid, gtasa_SL_IID_BUFFERQUEUE) || IID_IS(iid, gtasa_SL_IID_ANDROIDSIMPLEBUFFERQUEUE)) {
    Player *p = (Player *)self; *(void **)pItf = &p->bqItf; return SL_RESULT_SUCCESS;
  }
  if (pItf) *(void **)pItf = NULL;
  return 0x1108 /*SL_RESULT_FEATURE_UNSUPPORTED*/;
}
static void obj_Destroy(SLObjectItf self) {
  /* se for o player, fecha o SDL audio */
  if ((void *)self == (void *)&g_player.objItf && g_player.dev) {
    SDL_CloseAudioDevice(g_player.dev); g_player.dev = 0;
  }
}
static SLresult obj_ret0(void) { return SL_RESULT_SUCCESS; }
static const void *obj_vtbl[] = {
  obj_Realize, obj_ret0 /*Resume*/, obj_GetState, obj_GetInterface,
  obj_ret0 /*RegisterCallback*/, obj_ret0 /*AbortAsync*/, obj_Destroy,
  obj_ret0 /*SetPriority*/, obj_ret0 /*GetPriority*/, obj_ret0 /*SetLossOfControl*/,
};

/* ---- SLEngineItf ---- */
static SLresult eng_CreateAudioPlayer(SLEngineItf self, SLObjectItf *pPlayer,
                                      SLDataSource *src, SLDataSink *snk,
                                      SLuint32 n, const void *ids, const void *req) {
  (void)self; (void)snk; (void)n; (void)ids; (void)req;
  Player *p = &g_player;
  memset(p, 0, sizeof(*p));
  p->objItf = (const void*)obj_vtbl; p->playItf = (const void*)play_vtbl; p->bqItf = (const void*)bq_vtbl;
  pthread_mutex_init(&p->lock, NULL);
  p->channels = 2; p->rate = 44100; p->bits = 16;
  if (src && src->pFormat) {
    SLDataFormat_PCM *f = (SLDataFormat_PCM *)src->pFormat;
    if (f->numChannels) p->channels = f->numChannels;
    if (f->samplesPerSec) p->rate = f->samplesPerSec / 1000; /* milliHz -> Hz */
    if (f->bitsPerSample) p->bits = f->bitsPerSample;
  }
  SDL_AudioSpec want, have;
  memset(&want, 0, sizeof(want));
  want.freq = p->rate; want.format = (p->bits == 8) ? AUDIO_U8 : AUDIO_S16SYS;
  want.channels = p->channels; want.samples = 1024;
  want.callback = sdl_audio_cb; want.userdata = p;
  if (SDL_WasInit(SDL_INIT_AUDIO) == 0) SDL_InitSubSystem(SDL_INIT_AUDIO);
  p->dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
  fprintf(stderr, "[opensl] AudioPlayer %dch %dHz %dbit -> SDL dev=%u (%s)\n",
          p->channels, p->rate, p->bits, p->dev, p->dev ? "OK" : SDL_GetError());
  *pPlayer = (SLObjectItf)&p->objItf;
  return SL_RESULT_SUCCESS;
}
static SLresult eng_CreateOutputMix(SLEngineItf self, SLObjectItf *pMix,
                                    SLuint32 n, const void *ids, const void *req) {
  (void)self; (void)n; (void)ids; (void)req;
  static OutputMix mix; mix.objItf = (const void*)obj_vtbl;
  *pMix = (SLObjectItf)&mix.objItf;
  return SL_RESULT_SUCCESS;
}
static SLresult eng_ret0(void) { return SL_RESULT_SUCCESS; }
static const void *eng_vtbl[] = {
  eng_ret0 /*CreateLEDDevice*/, eng_ret0 /*CreateVibra*/, eng_CreateAudioPlayer,
  eng_ret0 /*CreateAudioRecorder*/, eng_ret0 /*CreateMidiPlayer*/, eng_ret0 /*CreateListener*/,
  eng_ret0 /*Create3DGroup*/, eng_CreateOutputMix, eng_ret0 /*CreateMetadataExtractor*/,
  eng_ret0 /*CreateExtensionObject*/, eng_ret0, eng_ret0, eng_ret0, eng_ret0, eng_ret0,
};

/* ---- slCreateEngine ---- */
static Engine g_engine;
SLresult gtasa_slCreateEngine(SLObjectItf *pEngine, SLuint32 numOptions, const void *pOptions,
                              SLuint32 numInterfaces, const void *pInterfaceIds, const void *pInterfaceRequired) {
  (void)numOptions; (void)pOptions; (void)numInterfaces; (void)pInterfaceIds; (void)pInterfaceRequired;
  g_engine.objItf = (const void*)obj_vtbl; g_engine.engItf = (const void*)eng_vtbl;
  *pEngine = (SLObjectItf)&g_engine.objItf;
  fprintf(stderr, "[opensl] slCreateEngine -> shim (SDL_Audio backend)\n");
  return SL_RESULT_SUCCESS;
}

/* SL_IID_* : ponteiros distintos (o valor não importa; só a identidade). O jogo
 * importa SL_IID_* como DADO (ponteiro pra interface id); expomos estes. */
const void *const gtasa_SL_IID_ENGINE = (const void *)"ENGINE";
const void *const gtasa_SL_IID_PLAY = (const void *)"PLAY";
const void *const gtasa_SL_IID_BUFFERQUEUE = (const void *)"BQ";
const void *const gtasa_SL_IID_ANDROIDSIMPLEBUFFERQUEUE = (const void *)"ANDROIDBQ";
