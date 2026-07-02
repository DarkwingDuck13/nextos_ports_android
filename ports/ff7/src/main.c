/*
 * FINAL FANTASY VII (engine materialg / SQEX, jp.co.d4e) -> aarch64 Linux
 * so-loader para Mali-450 fbdev.
 *
 * Carrega libjni_ff7.so (STL estatica, sem aux), monta JNIEnv falso e dirige o
 * GLESJniWrapper sem ART:
 *   JNI_OnLoad -> setAssetManager -> setDataPath -> setLang
 *   -> onSurfaceCreated -> onSurfaceChanged(w,h) -> loop onDrawFrame
 * Assets do APK (Shaders/AVConfig) via AAsset shim (./assets/...).
 * Dados do jogo (OBB extraido) via setDataPath -> fopen <datapath>/ff7_1.02/...
 * Audio: OpenSLES shim. Input: SDL -> onKey/onTouch.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <SDL2/SDL.h>

#include "error.h"
#include "imports.h"
#include "jni_shim.h"
#include "so_util.h"
#include "util.h"
#include "fmv.h"

/* GL (libGLESv2) p/ a injecao de frame de FMV na textura 2D */
extern void glGenTextures(int, unsigned *);
extern void glBindTexture(unsigned, unsigned);
extern void glTexParameteri(unsigned, unsigned, int);
extern void glTexImage2D(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
extern void glTexSubImage2D(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
extern void glActiveTexture(unsigned);

typedef int jint;
typedef unsigned char jboolean;
typedef float jfloat;

#define MEMORY_MB 320
#define SO_NAME "libjni_ff7.so"

/* ---- Android keycodes (onKey usa keycodes Android) ---- */
#define AKEYCODE_DPAD_UP 19
#define AKEYCODE_DPAD_DOWN 20
#define AKEYCODE_DPAD_LEFT 21
#define AKEYCODE_DPAD_RIGHT 22
#define AKEYCODE_DPAD_CENTER 23
#define AKEYCODE_BUTTON_A 96
#define AKEYCODE_BUTTON_B 97
#define AKEYCODE_BUTTON_X 99
#define AKEYCODE_BUTTON_Y 100
#define AKEYCODE_BUTTON_L1 102
#define AKEYCODE_BUTTON_R1 103
#define AKEYCODE_BUTTON_START 108
#define AKEYCODE_BUTTON_SELECT 109
#define AKEYCODE_ENTER 66
#define AKEYCODE_BACK 4

/* ---- GLESJniWrapper (jp.co.d4e.materialg) ---- */
static jint (*JNI_OnLoad)(void *vm, void *reserved);
static void (*setAssetManager)(void *env, void *clazz, void *assetmgr);
static void (*setDataPath)(void *env, void *clazz, void *jstr);
static void (*setLang)(void *env, void *clazz, jint lang);
static void (*onSurfaceCreated)(void *env, void *clazz);
static void (*onSurfaceChanged)(void *env, void *clazz, jint w, jint h);
static void (*onDrawFrame)(void *env, void *clazz);
static void (*onResume)(void *env, void *clazz);
static void (*onPause)(void *env, void *clazz);
static void (*onKey)(void *env, void *clazz, jint keycode, jboolean pressed);
static void (*onKeyBack)(void *env, void *clazz);
static void (*onTouchBegan)(void *env, void *clazz, jfloat x, jfloat y);
static void (*onTouchMoved)(void *env, void *clazz, jfloat x, jfloat y);
static void (*onTouchEnded)(void *env, void *clazz, jfloat x, jfloat y);
static void (*callUpdateTitlemenu)(void *env, void *clazz);
static void (*g_fw_stop_movie)(void);   /* _Z13fw_stop_moviev: encerra o FMV em curso */
static void (*setBatteryLevel)(void *env, void *clazz, jint lvl);
static void (*VIDEO_update)(void);  /* render real single-thread (bypassa worker) */
/* SQEX CoreSystem master volume (DynamicValue global). SetMasterVolume e' "static"
 * (ignora this, usa global) -> chamavel como (float vol, unsigned flag). A musica
 * (AKB Ogg via SQEX CoreSource) e' multiplicada por esse master no RenderMix; se
 * ficar 0 (init default, nunca setado), a BGM/audio de batalha sai MUDA. */
static float (*g_GetMasterVolume)(void);
static void  (*g_SetMasterVolume)(float vol, unsigned flag);
/* SoundSystem_Update: tick do sistema de som mobile (SQEX). Normalmente um THREAD
 * timer (SoundSystem::CreateUpdateTimer) o chama periodicamente; no nosso ambiente
 * esse timer NAO dispara, entao o streaming de BGM (StreamingSound::Update ->
 * decode Ogg -> QueueBuffer) drena depois dos 1os buffers e a musica fica MUDA.
 * Chamamos por frame (como callUpdateTitlemenu) p/ realimentar o streaming. */
static void (*g_SoundSystem_Update)(void);

static SDL_GameController *g_gamepad = NULL;
static void *g_env = NULL;

/* ---- Trampoline hook (diag audio): chama o original e mede a saida ---- */
typedef void (*rendermix_fn)(void *thisp, void *out, unsigned size);
static rendermix_fn g_orig_rendermix = NULL;

static void *make_trampoline(uintptr_t fn, int nbytes) {
  void *tr = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (tr == MAP_FAILED) return NULL;
  memcpy(tr, (void *)fn, nbytes);             /* prologo original (relocavel) */
  uint32_t *p = (uint32_t *)((char *)tr + nbytes);
  p[0] = 0x58000051u;                          /* LDR X17, #8 */
  p[1] = 0xd61f0220u;                          /* BR X17 */
  *(uint64_t *)(p + 2) = fn + nbytes;          /* continua em fn+nbytes */
  __builtin___clear_cache((char *)tr, (char *)tr + 4096);
  return tr;
}

extern void ff7_music_feed(const void *pcm, unsigned bytes);
static int g_music_bypass = 0;

/* ---- BGM via PCM (bypass do streaming SQEX flaky) ---- */
extern void ff7_bgm_pcm_start(void);
extern void ff7_bgm_pcm_feed(const void *pcm, unsigned bytes);
extern unsigned ff7_bgm_readable(void);
extern void ff7_bgm_pcm_stop(void);
static FILE *g_bgm_f = NULL;
static char g_bgm_pcm_path[1024]; /* .pcm pendente (ativado tarde p/ evitar deadlock Mali no boot) */
static char g_bgm_cur[1024];      /* path .akb atual (evita reabrir o mesmo) */
static int g_bgm_active = 0;
static int g_bgm_pending = 0;
static uint8_t g_bgm_buf[65536];

/* chamado de imports.c quando music_2/<nome>.akb abre: SO' registra o .pcm pendente
 * (a ativacao real e' adiada p/ depois do boot — ff7_bgm_maybe_activate). */
void ff7_bgm_set_akb(const char *akbpath) {
  if (!akbpath || !getenv("FF7_BGMPCM")) return; /* OPT-IN (default OFF p/ build seguro) */
  if (strcmp(akbpath, g_bgm_cur) == 0) return; /* mesma musica */
  char p[1024]; snprintf(p, sizeof p, "%s", akbpath);
  char *d = strrchr(p, '.'); if (d) snprintf(d, sizeof(p) - (d - p), ".pcm");
  /* SEM fopen aqui: durante o boot (render fragil do Mali) qualquer I/O extra pode
   * perturbar o threading -> deadlock. So' registra o path; valida/abre na ativacao. */
  snprintf(g_bgm_pcm_path, sizeof g_bgm_pcm_path, "%s", p);
  snprintf(g_bgm_cur, sizeof g_bgm_cur, "%s", akbpath);
  g_bgm_pending = 1;
}

/* ativa a BGM pendente (chamado do present cb SO' apos o boot estabilizar — frame
 * grande — pq ativar um player 44100 durante o render fragil do boot deadlocka o Mali). */
static void ff7_bgm_maybe_activate(void) {
  if (!g_bgm_pending) return;
  if (g_bgm_f) fclose(g_bgm_f);
  g_bgm_f = fopen(g_bgm_pcm_path, "rb");
  g_bgm_pending = 0;
  if (!g_bgm_f) return;
  g_bgm_active = 1;
  ff7_bgm_pcm_start();
  debugPrintf("BGM: ATIVADO PCM %s (loop)\n", g_bgm_pcm_path);
}

/* por frame: mantem o ring da BGM cheio (~0.4s), em loop. */
static void ff7_bgm_pump(void) {
  if (!g_bgm_active || !g_bgm_f) return;
  unsigned target = 44100u * 4u * 40u / 100u; /* ~0.4s @44100 stereo s16 */
  int guard = 8; /* bound CONSERVADOR: no max 8*64KB=512KB/frame */
  static int dbg = 0;
  unsigned r0 = ff7_bgm_readable();
  while (ff7_bgm_readable() < target && guard-- > 0) {
    size_t got = fread(g_bgm_buf, 1, sizeof g_bgm_buf, g_bgm_f);
    if (got == 0) { fseek(g_bgm_f, 0, SEEK_SET); continue; } /* loop a faixa */
    ff7_bgm_pcm_feed(g_bgm_buf, (unsigned)got);
  }
  if (dbg < 6) { debugPrintf("BGMPUMP r0=%u r1=%u guard=%d\n", r0, ff7_bgm_readable(), guard); dbg++; }
}

/* ---- FMV: injeta o frame VP8 decodificado (fmv.c) numa textura 2D e forca o
 * shader p/ enableOES=0 (sampler2D), driblando o samplerExternalOES que o Mali
 * fbdev nao alimenta. Hook em VIDEO_enableOES: so' filmes chamam com en=1. ---- */
typedef void (*enableoes_fn)(int);
static enableoes_fn g_orig_enableOES = NULL;
/* DIAG refill SQEX: conta CoreSource::QueueBuffer (refill da fonte de musica). */
typedef void (*queuebuf_fn)(void *thisp, void *buf, unsigned size);
static queuebuf_fn g_orig_queuebuf = NULL;
static void my_QueueBuffer(void *thisp, void *buf, unsigned size) {
  static unsigned qc = 0; qc++;
  if (qc <= 12 || qc % 50 == 0) {
    int pk = 0; const short *s = (const short *)buf;
    for (unsigned i = 0; i < size / 2 && i < 8192; i++) { int a = s[i] < 0 ? -s[i] : s[i]; if (a > pk) pk = a; }
    debugPrintf("QBUF #%u size=%u peak=%d\n", qc, size, pk);
  }
  /* FF7_BGMDUMP: grava os primeiros ~40 buffers enfileirados (dado que DEVERIA tocar) */
  if (getenv("FF7_BGMDUMP") && qc <= 40) {
    static FILE *qf = NULL;
    if (!qf) qf = fopen("qbuf.raw", "wb");
    if (qf) { fwrite(buf, 1, size, qf); fflush(qf); }
  }
  g_orig_queuebuf(thisp, buf, size);
}
typedef int (*playmidi_fn)(unsigned, unsigned, unsigned);
static playmidi_fn g_orig_playmidi = NULL;
static int my_playmidi(unsigned a, unsigned b, unsigned c) {
  debugPrintf("PLAYMIDI music req a=%u b=%u c=%u\n", a, b, c);
  return g_orig_playmidi(a, b, c);
}
typedef void (*avi_open_fn)(const char *);
static avi_open_fn g_orig_avi_open = NULL;
static void my_avi_open(const char *name) {
  fmv_set_movie_by_name(name);   /* captura qual filme p/ nosso decode VP8 */
  if (g_orig_avi_open) g_orig_avi_open(name);
}
static unsigned g_fmv_tex = 0;
static int g_fmv_tex_w = 0, g_fmv_tex_h = 0;
#define GL_TEXTURE_2D 0x0DE1
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_TEXTURE0 0x84C0
#define GL_TEX_MIN 0x2801
#define GL_TEX_MAG 0x2800
#define GL_LINEAR 0x2601
#define GL_WRAP_S 0x2802
#define GL_WRAP_T 0x2803
#define GL_CLAMP_EDGE 0x812F

extern long g_present_frame_n; /* setado no present cb = g_frame */
static unsigned g_fmv_texN[3]; static int g_fmv_texi;  /* triple-buffer */
static int g_fmv_alloc_w, g_fmv_alloc_h;
extern int fmv_total_frames(void);
extern int fmv_cur_frame(void);
extern int fmv_fps(void);
int g_ff7_fmv_inject = 0;   /* FF7_FMVINJECT: modo antigo (hijack enableOES) */

/* ---- CLOCK do FMV (modo overlay): avanca o decode pela fps REAL do video.
 * Chamado (a) dos handlers MyDecoder FRAME/GET_POSITION — que rodam DENTRO do
 * wait-loop de fw_update_movie_sample na main thread (o engine BLOQUEIA ali
 * esperando o proximo frame; sem isso = deadlock engine<->present) — e (b) do
 * present cb. So' CPU (decode VP8 + feed de audio), nada de GL. ---- */
void ff7_fmv_clock_tick(void) {
  extern int g_ff7_movie_active;
  if (g_ff7_fmv_inject || getenv("FF7_NOFMV")) return;
  if (!g_ff7_movie_active || !fmv_has_movie() || fmv_eof()) return;
  const char *mn = fmv_current_name();
  if (!mn || !mn[0] || strstr(mn, "logo")) return;
  static char cur_movie[256];
  static unsigned start_ticks;
  unsigned now = SDL_GetTicks();
  if (strncmp(cur_movie, mn, sizeof cur_movie - 1) != 0) {
    snprintf(cur_movie, sizeof cur_movie, "%s", mn);
    start_ticks = now;
    debugPrintf("FMVCLK: filme '%s' total=%d fps=%d\n", mn, fmv_total_frames(), fmv_fps());
  }
  if (fmv_cur_frame() == 0) start_ticks = now;  /* 1o frame ancora o relogio */
  int target = (int)((unsigned long)(now - start_ticks) * (unsigned)fmv_fps() / 1000u) + 1;
  int tot = fmv_total_frames();
  if (tot > 0 && target > tot) target = tot;
  int budget = 4;
  while (fmv_cur_frame() < target && !fmv_eof() && budget-- > 0)
    if (!fmv_next_frame()) break;
}

static int fmv_decode_and_bind(void) {
  /* modo overlay: NAO decodifica aqui (o clock tick ja' decodificou); so' sobe a
   * textura quando ha' frame NOVO e binda. No modo inject (antigo) decodifica
   * 1x/present como antes. */
  static long last_dec = -1;
  static int last_up = -1;
  int got = 0;
  if (!g_ff7_fmv_inject) {
    if (fmv_cur_frame() == 0) return 0;               /* nada decodificado ainda */
    got = (fmv_cur_frame() != last_up);               /* frame novo -> re-upload */
    if (got) last_up = fmv_cur_frame();
  } else {
    if (g_fmv_texN[g_fmv_texi] && last_dec == g_present_frame_n) {
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, g_fmv_texN[g_fmv_texi]);
      return 1;
    }
    last_dec = g_present_frame_n;
    got = fmv_next_frame();
  }
  if (!g_fmv_texN[0] && !got) return 0;
  int w = fmv_w(), h = fmv_h();
  /* TRIPLE-BUFFER: sobe pra uma textura que o GPU NAO esta' lendo (a do quad do
   * frame anterior). glTexSubImage2D numa textura em uso pelo GPU -> Mali emperra
   * (stall no _mali_convert/wait) = travamentos. Alterna 3 texturas p/ evitar. */
  if (!g_fmv_texN[0]) {
    glGenTextures(3, g_fmv_texN);
    for (int i = 0; i < 3; i++) {
      glBindTexture(GL_TEXTURE_2D, g_fmv_texN[i]);
      glTexParameteri(GL_TEXTURE_2D, GL_TEX_MIN, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEX_MAG, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_WRAP_S, GL_CLAMP_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_WRAP_T, GL_CLAMP_EDGE);
    }
  }
  if (got) g_fmv_texi = (g_fmv_texi + 1) % 3; /* proxima textura (nao a renderizada) */
  unsigned tex = g_fmv_texN[g_fmv_texi];
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, tex);
  const unsigned char *pix = fmv_rgba();
  static unsigned char *redbuf = NULL;
  if (getenv("FF7_FMVRED")) {
    if (!redbuf) { redbuf = malloc((size_t)w * h * 4); for (int k = 0; k < w * h; k++) { redbuf[k*4]=255; redbuf[k*4+1]=0; redbuf[k*4+2]=0; redbuf[k*4+3]=255; } }
    pix = redbuf; got = 1;
  }
  if (got) {
    /* sempre glTexImage2D (orphan): aloca buffer novo no driver -> NUNCA espera o
     * GPU terminar de ler o buffer antigo = sem stall. (custa um pouco mais de RAM) */
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pix);
    g_fmv_alloc_w = w; g_fmv_alloc_h = h; (void)g_fmv_tex_w; (void)g_fmv_tex_h;
  }
  return 1;
}

/* ==== FMV OVERLAY DESACOPLADO (default; FF7_FMVINJECT volta ao modo antigo) ====
 * O modo antigo (hijack do VIDEO_enableOES) corrompia a maquina de estado do
 * filme no engine -> o filme nunca encerrava -> script do campo travava esperando
 * = TELA PRETA pos-trem. Aqui o engine toca o filme SOZINHO (quad OES preto,
 * intocado — igual ao NOFMV que comprovadamente transiciona limpo pro campo) e
 * nos desenhamos o video decodificado POR CIMA, no present cb, salvando e
 * restaurando TODO o estado GL que tocamos (a falta do restore foi o que
 * quebrou/zoomou o render na 1a tentativa). */
extern void glCreateShaderStub(void); /* marcador; declaracoes reais abaixo */
extern unsigned glCreateShader(unsigned type);
extern void glShaderSource(unsigned sh, int n, const char **src, const int *len);
extern void glCompileShader(unsigned sh);
extern void glGetShaderiv(unsigned sh, unsigned pname, int *out);
extern unsigned glCreateProgram(void);
extern void glAttachShader(unsigned prog, unsigned sh);
extern void glBindAttribLocation(unsigned prog, unsigned idx, const char *name);
extern void glLinkProgram(unsigned prog);
extern void glGetProgramiv(unsigned prog, unsigned pname, int *out);
extern void glUseProgram(unsigned prog);
extern int  glGetUniformLocation(unsigned prog, const char *name);
extern void glUniform1i(int loc, int v);
extern void glVertexAttribPointer(unsigned idx, int size, unsigned type, unsigned char norm, int stride, const void *ptr);
extern void glEnableVertexAttribArray(unsigned idx);
extern void glDisableVertexAttribArray(unsigned idx);
extern void glDrawArrays(unsigned mode, int first, int count);
extern void glGetIntegerv(unsigned pname, int *out);
extern void glGetBooleanv(unsigned pname, unsigned char *out);
extern void glGetVertexAttribiv(unsigned idx, unsigned pname, int *out);
extern void glGetVertexAttribPointerv(unsigned idx, unsigned pname, void **out);
extern unsigned char glIsEnabled(unsigned cap);
extern void glEnable(unsigned cap);
extern void glDisable(unsigned cap);
extern void glViewport(int x, int y, int w, int h);
extern void glBindBuffer(unsigned target, unsigned buf);
extern void glColorMask(unsigned char r, unsigned char g, unsigned char b, unsigned char a);
extern void glBindFramebuffer(unsigned target, unsigned fb);

#define GLC_CURRENT_PROGRAM 0x8B8D
#define GLC_ACTIVE_TEXTURE 0x84E0
#define GLC_TEXTURE_BINDING_2D 0x8069
#define GLC_ARRAY_BUFFER 0x8892
#define GLC_ARRAY_BUFFER_BINDING 0x8894
#define GLC_VIEWPORT 0x0BA2
#define GLC_BLEND 0x0BE2
#define GLC_DEPTH_TEST 0x0B71
#define GLC_SCISSOR_TEST 0x0C11
#define GLC_CULL_FACE 0x0B44
#define GLC_STENCIL_TEST 0x0B90
#define GLC_COLOR_WRITEMASK 0x0C23
#define GLC_FRAMEBUFFER 0x8D40
#define GLC_FRAMEBUFFER_BINDING 0x8CA6
#define GLC_VAA_ENABLED 0x8622
#define GLC_VAA_SIZE 0x8623
#define GLC_VAA_STRIDE 0x8624
#define GLC_VAA_TYPE 0x8625
#define GLC_VAA_NORMALIZED 0x886A
#define GLC_VAA_POINTER 0x8645
#define GLC_VAA_BUFBIND 0x889F
#define GLC_FRAGMENT_SHADER 0x8B30
#define GLC_VERTEX_SHADER 0x8B31
#define GLC_COMPILE_STATUS 0x8B81
#define GLC_LINK_STATUS 0x8B82
#define GLC_TRIANGLE_STRIP 0x0005
#define GLC_FLOAT 0x1406

static int g_w, g_h;   /* tentative; definido (=0) mais abaixo */
static unsigned g_ovl_prog = 0;
static int g_ovl_prog_fail = 0;
static int g_ovl_utex = -1;

static int ovl_build_program(void) {
  if (g_ovl_prog) return 1;
  if (g_ovl_prog_fail) return 0;
  static const char *vs_src =
    "attribute vec2 aPos; attribute vec2 aTex; varying vec2 vT;\n"
    "void main(){ gl_Position = vec4(aPos, 0.0, 1.0); vT = aTex; }\n";
  static const char *fs_src =
    "precision mediump float; varying vec2 vT; uniform sampler2D uTex;\n"
    "void main(){ gl_FragColor = vec4(texture2D(uTex, vT).rgb, 1.0); }\n";
  unsigned vs = glCreateShader(GLC_VERTEX_SHADER);
  unsigned fs = glCreateShader(GLC_FRAGMENT_SHADER);
  int ok = 0;
  glShaderSource(vs, 1, &vs_src, NULL); glCompileShader(vs);
  glShaderSource(fs, 1, &fs_src, NULL); glCompileShader(fs);
  glGetShaderiv(vs, GLC_COMPILE_STATUS, &ok);
  int ok2 = 0; glGetShaderiv(fs, GLC_COMPILE_STATUS, &ok2);
  if (!ok || !ok2) { debugPrintf("FMVOVL: shader compile falhou (%d/%d)\n", ok, ok2); g_ovl_prog_fail = 1; return 0; }
  unsigned p = glCreateProgram();
  glAttachShader(p, vs); glAttachShader(p, fs);
  glBindAttribLocation(p, 0, "aPos");
  glBindAttribLocation(p, 1, "aTex");
  glLinkProgram(p);
  ok = 0; glGetProgramiv(p, GLC_LINK_STATUS, &ok);
  if (!ok) { debugPrintf("FMVOVL: link falhou\n"); g_ovl_prog_fail = 1; return 0; }
  g_ovl_prog = p;
  glUseProgram(p);
  g_ovl_utex = glGetUniformLocation(p, "uTex");
  debugPrintf("FMVOVL: program ok (prog=%u uTex=%d)\n", p, g_ovl_utex);
  return 1;
}

/* estado salvo dos vertex-attribs 0/1 */
typedef struct { int enabled, size, stride, type, norm, bufbind; void *ptr; } vaa_state;
static void vaa_save(unsigned i, vaa_state *s) {
  glGetVertexAttribiv(i, GLC_VAA_ENABLED, &s->enabled);
  glGetVertexAttribiv(i, GLC_VAA_SIZE, &s->size);
  glGetVertexAttribiv(i, GLC_VAA_STRIDE, &s->stride);
  glGetVertexAttribiv(i, GLC_VAA_TYPE, &s->type);
  glGetVertexAttribiv(i, GLC_VAA_NORMALIZED, &s->norm);
  glGetVertexAttribiv(i, GLC_VAA_BUFBIND, &s->bufbind);
  glGetVertexAttribPointerv(i, GLC_VAA_POINTER, &s->ptr);
}
static void vaa_restore(unsigned i, const vaa_state *s) {
  glBindBuffer(GLC_ARRAY_BUFFER, (unsigned)s->bufbind);
  if (s->size)
    glVertexAttribPointer(i, s->size, (unsigned)s->type, (unsigned char)s->norm, s->stride, s->ptr);
  if (s->enabled) glEnableVertexAttribArray(i); else glDisableVertexAttribArray(i);
}

/* pacing: decodifica ate' acompanhar o contador do movie do ENGINE (relativo ao
 * inicio deste filme) — video/audio seguem o relogio do engine e o GET_POSITION
 * (cur_frame) avanca coerente ate' o total. */
static unsigned g_ovl_base_mc = 0;
static int g_ovl_have_base = 0;
static char g_ovl_movie[256];
static long g_ovl_frames_drawn = 0;

void fmv_overlay_reset(void) { g_ovl_have_base = 0; g_ovl_movie[0] = 0; g_ovl_frames_drawn = 0; }

static void fmv_overlay_draw(void) {
  extern int g_ff7_movie_active;
  if (g_ff7_fmv_inject || getenv("FF7_NOFMV")) return;
  if (!g_ff7_movie_active) { g_ovl_have_base = 0; return; }  /* rearma p/ o proximo filme */
  if (!fmv_has_movie() || fmv_eof()) return;
  const char *mn = fmv_current_name();
  if (!mn || !mn[0] || strstr(mn, "logo")) return;
  if (!text_base) return;
  unsigned mc = *(unsigned *)((uintptr_t)text_base + 0x1cd8c9c);
  if (mc > 0x40000000u) return;
  if (!g_ovl_have_base || strncmp(g_ovl_movie, mn, sizeof g_ovl_movie - 1) != 0) {
    g_ovl_base_mc = mc; g_ovl_have_base = 1; g_ovl_frames_drawn = 0;
    snprintf(g_ovl_movie, sizeof g_ovl_movie, "%s", mn);
    debugPrintf("FMVOVL: filme '%s' base_mc=%u total=%d fps=%d\n",
                mn, mc, fmv_total_frames(), fmv_fps());
  }
  /* decode = ff7_fmv_clock_tick (relogio na fps real; chamado tb dos handlers
   * MyDecoder dentro do wait-loop do engine). Aqui so' garante o tick por
   * present e desenha o frame corrente. */
  ff7_fmv_clock_tick();
  if (fmv_cur_frame() == 0 || !fmv_rgba()) return;   /* nada decodificado ainda */

  /* ---- SALVA todo o estado GL que vamos tocar ---- */
  int prev_prog = 0, prev_active = 0, prev_tex0 = 0, prev_ab = 0, prev_fb = 0;
  int prev_vp[4] = {0,0,0,0};
  unsigned char prev_cmask[4];
  unsigned char en_blend, en_depth, en_scissor, en_cull, en_stencil;
  vaa_state va0, va1;
  glGetIntegerv(GLC_CURRENT_PROGRAM, &prev_prog);
  glGetIntegerv(GLC_ACTIVE_TEXTURE, &prev_active);
  glGetIntegerv(GLC_ARRAY_BUFFER_BINDING, &prev_ab);
  glGetIntegerv(GLC_FRAMEBUFFER_BINDING, &prev_fb);
  glGetIntegerv(GLC_VIEWPORT, prev_vp);
  glGetBooleanv(GLC_COLOR_WRITEMASK, prev_cmask);
  en_blend = glIsEnabled(GLC_BLEND); en_depth = glIsEnabled(GLC_DEPTH_TEST);
  en_scissor = glIsEnabled(GLC_SCISSOR_TEST); en_cull = glIsEnabled(GLC_CULL_FACE);
  en_stencil = glIsEnabled(GLC_STENCIL_TEST);
  vaa_save(0, &va0); vaa_save(1, &va1);
  glActiveTexture(GL_TEXTURE0);
  glGetIntegerv(GLC_TEXTURE_BINDING_2D, &prev_tex0);

  /* ---- desenha ---- */
  if (!ovl_build_program()) goto restore;
  glBindFramebuffer(GLC_FRAMEBUFFER, 0);
  if (!fmv_decode_and_bind()) goto restore;  /* sobe/binda a textura (cap 1/present) */
  glUseProgram(g_ovl_prog);
  if (g_ovl_utex >= 0) glUniform1i(g_ovl_utex, 0);
  glDisable(GLC_BLEND); glDisable(GLC_DEPTH_TEST); glDisable(GLC_SCISSOR_TEST);
  glDisable(GLC_CULL_FACE); glDisable(GLC_STENCIL_TEST);
  glColorMask(1, 1, 1, 1);
  int W = g_w > 0 ? g_w : 1280, H = g_h > 0 ? g_h : 720;
  glViewport(0, 0, W, H);
  /* aspecto: encaixa o filme (fmv_w x fmv_h) dentro da tela, centrado */
  float sx = 1.0f, sy = 1.0f;
  int fw = fmv_w(), fh = fmv_h();
  if (fw > 0 && fh > 0) {
    float sc_w = (float)W / fw, sc_h = (float)H / fh;
    float sc = sc_w < sc_h ? sc_w : sc_h;
    sx = (fw * sc) / W; sy = (fh * sc) / H;
  }
  float pos[8] = { -sx, -sy,  sx, -sy,  -sx, sy,  sx, sy };
  /* textura: linha 0 = topo do video -> V invertido no quad (NDC y cresce p/ cima) */
  float tex[8] = { 0.f, 1.f,  1.f, 1.f,  0.f, 0.f,  1.f, 0.f };
  glBindBuffer(GLC_ARRAY_BUFFER, 0);
  glVertexAttribPointer(0, 2, GLC_FLOAT, 0, 0, pos);
  glVertexAttribPointer(1, 2, GLC_FLOAT, 0, 0, tex);
  glEnableVertexAttribArray(0); glEnableVertexAttribArray(1);
  glDrawArrays(GLC_TRIANGLE_STRIP, 0, 4);
  g_ovl_frames_drawn++;
  if (g_ovl_frames_drawn <= 4 || g_ovl_frames_drawn % 200 == 0)
    debugPrintf("FMVOVL: draw #%ld mc=%u dec=%d/%d %dx%d\n",
                g_ovl_frames_drawn, mc, fmv_cur_frame(), fmv_total_frames(), fw, fh);

restore:
  /* ---- RESTAURA tudo ---- */
  glBindFramebuffer(GLC_FRAMEBUFFER, (unsigned)prev_fb);
  vaa_restore(0, &va0); vaa_restore(1, &va1);
  glBindBuffer(GLC_ARRAY_BUFFER, (unsigned)prev_ab);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, (unsigned)prev_tex0);
  glActiveTexture((unsigned)prev_active);
  glUseProgram((unsigned)prev_prog);
  glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
  glColorMask(prev_cmask[0], prev_cmask[1], prev_cmask[2], prev_cmask[3]);
  if (en_blend) glEnable(GLC_BLEND); else glDisable(GLC_BLEND);
  if (en_depth) glEnable(GLC_DEPTH_TEST); else glDisable(GLC_DEPTH_TEST);
  if (en_scissor) glEnable(GLC_SCISSOR_TEST); else glDisable(GLC_SCISSOR_TEST);
  if (en_cull) glEnable(GLC_CULL_FACE); else glDisable(GLC_CULL_FACE);
  if (en_stencil) glEnable(GLC_STENCIL_TEST); else glDisable(GLC_STENCIL_TEST);
}

static void my_enableOES(int en) {
  /* GATE ESTREITO (era a CAUSA do campo preto / "cloud nao aparece"): injetar SO'
   * durante o playback REAL do movie de abertura — NAO nos logos de boot e NAO depois
   * do filme acabar. O gate antigo `fmv_has_movie()` ficava TRUE p/ logos e tb DEPOIS
   * do eof (ate o reset) -> se o campo/batalha chamasse VIDEO_enableOES, a gente
   * sequestrava (bind da textura do FMV + glActiveTexture) e corrompia o render do
   * campo. g_ff7_movie_active = setado pelo MyDecoder.START, zerado no RESET (segue
   * o estado do movie no engine); +!eof +nao-logo. */
  extern int g_ff7_movie_active;
  const char *mn = fmv_current_name();
  int playing = g_ff7_movie_active && fmv_has_movie() && !fmv_eof()
                && mn && !strstr(mn, "logo") && getenv("FF7_NOFMV") == NULL;
  if (en && playing && fmv_decode_and_bind()) {
    static unsigned ic = 0;
    if (ic < 6 || ic % 100 == 0) debugPrintf("FMVINJECT #%u (en=1 -> 2D %dx%d)\n", ic, fmv_w(), fmv_h());
    ic++;
    g_orig_enableOES(0); /* shader usa s_texture (nosso frame 2D), nao o OES */
    return;
  }
  g_orig_enableOES(en);
}

/* ---- FF7_BORDER: override da cor do bezel lateral (opt-in). O engine desenha a
 * borda como quad de COR gradiente via VIDEO_doQuad_color_forBorder(r1,g1,b1,
 * r2,g2,b2) — azul e' o default do port mobile. FF7_BORDER="r,g,b" (solido) ou
 * "r1,g1,b1,r2,g2,b2" (gradiente topo->base), 0-255. Ex: FF7_BORDER=0,0,0 preto. */
typedef void (*border_fn)(int,int,int,int,int,int);
static border_fn g_orig_border = NULL;
static int g_border_c[6];
static void my_border(int r1, int g1, int b1, int r2, int g2, int b2) {
  static unsigned bc = 0;
  if (bc++ < 2) debugPrintf("BORDER: chamado orig=(%d,%d,%d)-(%d,%d,%d) -> override\n",
                            r1, g1, b1, r2, g2, b2);
  g_orig_border(g_border_c[0], g_border_c[1], g_border_c[2],
                g_border_c[3], g_border_c[4], g_border_c[5]);
}

typedef void (*setvol_fn)(void *, float);
typedef void (*setvolmx_fn)(void *, float, float);
static setvol_fn g_orig_setvol = NULL;
static setvolmx_fn g_orig_setvolmx = NULL;
static void my_SetVolume(void *thisp, float v) {
  static unsigned c = 0; c++;
  if (c <= 20 || c % 100 == 0) debugPrintf("SETVOL #%u this=%p v=%f\n", c, thisp, v);
  g_orig_setvol(thisp, v);
}
static void my_SetVolumeMatrix(void *thisp, float l, float r) {
  static unsigned c = 0; c++;
  if (c <= 20 || c % 100 == 0) debugPrintf("SETVOLMX #%u this=%p l=%f r=%f\n", c, thisp, l, r);
  g_orig_setvolmx(thisp, l, r);
}
typedef void (*srcstop_fn)(void *);
static srcstop_fn g_orig_srcstop = NULL, g_orig_srcstart = NULL;
static void my_SourceStop(void *thisp) {
  static unsigned c = 0; c++;
  debugPrintf("SRCSTOP #%u this=%p (state era %d)\n", c, thisp, *(int *)((char *)thisp + 88));
  g_orig_srcstop(thisp);
}
static void my_SourceStart(void *thisp) {
  static unsigned c = 0; c++;
  debugPrintf("SRCSTART #%u this=%p\n", c, thisp);
  g_orig_srcstart(thisp);
}
/* ---- Taxa de saida do SQEX: o engine pede 32000 no CoreSystem::Initialize.
 * Com 32000, a BGM (AKB 44100) sofre RESAMPLE DUPLO (SQEX 44100->32000 + nosso
 * 32000->44100, ambos lineares) = aliasing = CHIADO constante na musica.
 * Forcamos 44100 (= SDL_OUTPUT_RATE): SQEX vira passthrough e o feed cai no
 * fast-path sem resample. FF7_SQEXRATE=N override; FF7_NOSQEX44K desliga. ---- */
int g_ff7_sqex_rate = 32000;   /* lido pelo ff7_music_feed (opensles_shim) */
typedef int (*coreinit_fn)(int, int);
static coreinit_fn g_orig_coreinit = NULL;
static int my_CoreInit(int rate, int ch) {
  int want = 44100;
  if (getenv("FF7_SQEXRATE")) want = atoi(getenv("FF7_SQEXRATE"));
  if (getenv("FF7_NOSQEX44K")) want = rate;
  if (want < 8000 || want > 48000) want = rate;
  debugPrintf("COREINIT: engine pediu rate=%d ch=%d -> usando %d\n", rate, ch, want);
  g_ff7_sqex_rate = want;
  return g_orig_coreinit(want, ch);
}

typedef void (*sdvol_fn)(int, int, float);
static sdvol_fn g_orig_sdvol = NULL;
static void my_SdVolume(int id, int trans, float vol) {
  static unsigned c = 0; c++;
  if (c <= 40 || c % 100 == 0) debugPrintf("SDVOL #%u id=%d trans=%d vol=%f\n", c, id, trans, vol);
  g_orig_sdvol(id, trans, vol);
}
static setvol_fn g_orig_setpitch = NULL, g_orig_setlpf = NULL, g_orig_setpan = NULL;
static void my_SetPitch(void *thisp, float v) {
  static unsigned c = 0; c++;
  if (c <= 20 || c % 100 == 0) debugPrintf("SETPITCH #%u this=%p v=%f\n", c, thisp, v);
  g_orig_setpitch(thisp, v);
}
static void my_SetIIRLPF(void *thisp, float v) {
  static unsigned c = 0; c++;
  if (c <= 20 || c % 100 == 0) debugPrintf("SETLPF #%u this=%p v=%f\n", c, thisp, v);
  g_orig_setlpf(thisp, v);
}
static void my_SetPan(void *thisp, float v) {
  static unsigned c = 0; c++;
  if (c <= 20 || c % 100 == 0) debugPrintf("SETPAN #%u this=%p v=%f\n", c, thisp, v);
  g_orig_setpan(thisp, v);
}

static void my_RenderMix(void *thisp, void *out, unsigned size) {
  /* FF7_BGMTRACE: estado interno do CoreSource da musica (callback de refill
   * [this+96], fila [this+116], idx [this+120], pos [this+124]). */
  int bgmtrace = getenv("FF7_BGMTRACE") != NULL;
  static unsigned btc = 0;
  int qn0 = 0, qi0 = 0, qp0 = 0;
  if (bgmtrace) { btc++;
    qn0 = *(int *)((char *)thisp + 116);
    qi0 = *(int *)((char *)thisp + 120);
    qp0 = *(int *)((char *)thisp + 124);
  }
  g_orig_rendermix(thisp, out, size);
  /* FF7_BGMDUMP: grava as primeiras ~200 saidas do RenderMix (o que TOCA) */
  if (getenv("FF7_BGMDUMP")) {
    static FILE *rf = NULL; static unsigned rn = 0;
    if (!rf && rn == 0) rf = fopen("rmout.raw", "wb");
    if (rf) { fwrite(out, 1, size, rf); fflush(rf); if (++rn >= 200) { fclose(rf); rf = NULL; rn = 1000; } }
  }
  if (bgmtrace && (btc <= 200 || btc % 100 == 0)) {
    int st = *(int *)((char *)thisp + 88);
    float vol = *(float *)((char *)thisp + 176);
    /* peak da saida + peak do buffer da fila corrente (o que DEVERIA tocar) */
    int opk = 0; { int n2 = size / 2; const short *s = (const short *)out;
      for (int i = 0; i < n2 && i < 4096; i++) { int a2 = s[i]<0?-s[i]:s[i]; if (a2>opk) opk=a2; } }
    /* NAO deref o ptr da fila (no 1o render e' LIXO -> segfault; o RenderMix le
     * a mesma struct = a fonte do RUIDO). So' loga os valores brutos. */
    unsigned long b0 = *(unsigned long *)((char *)thisp + 144);
    unsigned long b1 = *(unsigned long *)((char *)thisp + 160);
    unsigned long s0 = *(unsigned long *)((char *)thisp + 152);
    unsigned long s1 = *(unsigned long *)((char *)thisp + 168);
    debugPrintf("BGMTRACE #%u fila=%d idx=%d rest=%d st=%d vol=%.2f outpk=%d buf0=%#lx/%lu buf1=%#lx/%lu\n",
                btc, qn0, qi0, qp0, st, vol, opk, b0, s0, b1, s1);
  }
  /* BYPASS da BGM p/ o MUSIC_SLOT. ⚠️O RenderMix produz FLOAT32 (+-1.0), NAO
   * s16 — alimentar cru = RUIDO BRANCO (era o "chiado" nos creditos/gameplay,
   * NextOS 2026-07-02). Converte float->s16 com clamp antes de alimentar.
   * (player 0 nativo segue mudo: fill-cb do engine enfileira zeros.) */
  if (g_music_bypass && !g_bgm_active) {
    static short cvt[8192];
    const float *fsrc = (const float *)out;
    unsigned nf = size / 4;              /* size em BYTES; amostras float32 */
    if (nf > 8192) nf = 8192;
    for (unsigned i2 = 0; i2 < nf; i2++) {
      float v = fsrc[i2] * 32767.0f;
      if (v > 32767.0f) v = 32767.0f;
      if (v < -32768.0f) v = -32768.0f;
      cvt[i2] = (short)v;
    }
    ff7_music_feed(cvt, nf * 2);
  }
  if (getenv("FF7_RMDIAG")) {
    static unsigned cnt = 0;
    cnt++;
    if (cnt <= 8 || cnt % 200 == 1) {
      int pk = 0; int n = size / 2; const short *s = (const short *)out;
      for (int i = 0; i < n && i < 8192; i++) { int a = s[i] < 0 ? -s[i] : s[i]; if (a > pk) pk = a; }
      void *master = NULL;
      if (text_base) { void *st = *(void **)((char *)text_base + 0x133a000 + 2224);
        if (st) master = *(void **)st; }
      debugPrintf("RMDIAG #%u out=%p master=%p match=%d size=%u peak=%d\n",
                  cnt, out, master, (out == master), size, pk);
    }
  }
}

/* present single-thread: VIDEO_update chama sem_post(semB)/sem_wait(semA) no fim;
 * nossos wrappers (imports.c) chamam g_ff7_present_cb no sem_post(semB). */
extern void *g_ff7_present_semA, *g_ff7_present_semB;
extern void (*g_ff7_present_cb)(void);
extern void *text_base;  /* base de carga do .so (so_util) */
/* EGL do host (libEGL) p/ salvar os handles na struct de present do FF7 */
extern void *eglGetCurrentDisplay(void);
extern void *eglGetCurrentSurface(int readdraw);
extern void *eglGetCurrentContext(void);
extern unsigned eglMakeCurrent(void *dpy, void *draw, void *read, void *ctx);
#include <pthread.h>

static SDL_Window *g_window = NULL;
static SDL_GLContext g_glc = NULL;
static void *g_egl_dpy, *g_egl_surf, *g_egl_ctx;  /* p/ re-bind no present */
static int g_w = 0, g_h = 0;
static long g_frame = 0;
long g_present_frame_n = 0; /* espelho de g_frame p/ o cap de upload do FMV */
static long g_maxframes = 0;
static long g_shots_every = 0;
static int g_quit = 0;

/* CANARY BIONIC (SOTN/Bully/Dysmantle/Chrono): lib compilada p/ bionic le a
 * stack-canary de tpidr_el0+0x28 (TLS_SLOT_STACK_GUARD). Sob glibc esse offset
 * colide com TLS do Mali/SDL -> canary "muda" -> __stack_chk_fail falso. Pad
 * _Thread_local desloca o TLS estatico p/ tpidr+0x28 cair num pad nunca-escrito. */
__attribute__((used, aligned(16))) _Thread_local char g_bionic_guard_pad[256];

static SDL_GLContext gl_create_context_guarded(SDL_Window *w) {
  unsigned long tp; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
  unsigned long g = *(unsigned long *)(tp + 0x28);
  SDL_GLContext c = SDL_GL_CreateContext(w);
  *(unsigned long *)(tp + 0x28) = g;
  return c;
}
static void gl_swap_guarded(SDL_Window *w) {
  unsigned long tp; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
  unsigned long g = *(unsigned long *)(tp + 0x28);
  SDL_GL_SwapWindow(w);
  *(unsigned long *)(tp + 0x28) = g;
}

/* SDL controller/keyboard -> Android keycode */
static int map_btn_android(int b) {
  /* codigos INTERNOS do FF7 (tabela 0x132b260): 0=UP 1=LEFT 2=DOWN 3=RIGHT
   * 4=A(OK) 5=B(cancel) 6=X 7=Y 8=L1 9=R1 10=START 11=SELECT 12=L2 13=R2. */
  switch (b) {
    case SDL_CONTROLLER_BUTTON_A: return 4;   /* A = OK/confirmar */
    case SDL_CONTROLLER_BUTTON_B: return 5;   /* B = cancelar */
    case SDL_CONTROLLER_BUTTON_X: return 6;
    case SDL_CONTROLLER_BUTTON_Y: return 7;
    case SDL_CONTROLLER_BUTTON_START: return 10;
    case SDL_CONTROLLER_BUTTON_BACK: return 11;  /* SELECT */
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return 8;   /* L1 */
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return 9;  /* R1 */
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return 0;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return 2;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return 1;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return 3;
    default: return -1;
  }
}
static int map_key_android(SDL_Keycode k) {
  switch (k) {
    case SDLK_UP: return 0;
    case SDLK_LEFT: return 1;
    case SDLK_DOWN: return 2;
    case SDLK_RIGHT: return 3;
    case SDLK_SPACE: case SDLK_z: case SDLK_RETURN: return 4;  /* OK */
    case SDLK_LCTRL: case SDLK_x: case SDLK_BACKSPACE: return 5; /* cancel */
    case SDLK_LSHIFT: case SDLK_a: return 6;
    case SDLK_LALT: case SDLK_s: return 7;
    case SDLK_q: return 8;
    case SDLK_w: return 9;
    case SDLK_RETURN2: return 10;
    case SDLK_TAB: return 11;
    default: return -1;
  }
}

static void send_button(int sdl_button, int pressed) {
  if (!onKey) return;
  int kc = map_btn_android(sdl_button);
  if (kc >= 0) onKey(g_env, NULL, kc, pressed);
}

static void open_gamepad(void) {
  for (int i = 0; i < SDL_NumJoysticks(); i++) {
    if (SDL_IsGameController(i)) {
      g_gamepad = SDL_GameControllerOpen(i);
      if (g_gamepad) { debugPrintf("Gamepad: %s\n", SDL_GameControllerName(g_gamepad)); break; }
    }
  }
}

extern void opensles_shim_pump_callbacks(void) __attribute__((weak));

/* screenshot via glReadPixels (fb0 falha durante render Mali). */
extern void glReadPixels(int x, int y, int w, int h, unsigned fmt, unsigned type, void *px);
extern void glBindFramebuffer(unsigned target, unsigned fb);
extern void glGetIntegerv(unsigned pname, int *p);
static void ff7_dump_shot(int w, int h, int frame) {
  size_t n = (size_t)w * h * 4;
  unsigned char *buf = malloc(n);
  if (!buf) return;
  /* FF7_SHOTFB0: le o FRAMEBUFFER PADRAO (0) = exatamente o que vai pro swap/tela.
   * Sem isso, glReadPixels le o FBO bindado (pode mostrar o campo que NAO chega na
   * tela). NextOS ve preto pos-trem mas meu shot via FBO mostrava o campo -> testar. */
  int prevfb = -1;
  if (getenv("FF7_SHOTFB0")) {
    glGetIntegerv(0x8CA6 /*FRAMEBUFFER_BINDING*/, &prevfb);
    glBindFramebuffer(0x8D40 /*GL_FRAMEBUFFER*/, 0);
  }
  glReadPixels(0, 0, w, h, 0x1908 /*GL_RGBA*/, 0x1401 /*GL_UNSIGNED_BYTE*/, buf);
  if (prevfb >= 0) glBindFramebuffer(0x8D40, (unsigned)prevfb);
  const char *home = getenv("HOME"); if (!home) home = "/tmp";
  char path[256]; snprintf(path, sizeof path, "%s/ff7_shot_%04d.raw", home, frame);
  FILE *f = fopen(path, "wb");
  if (f) { fwrite(buf, 1, n, f); fclose(f); debugPrintf("SHOT %s (%dx%d)\n", path, w, h); }
  free(buf);
}

/* FF7 roda o .exe do FF7 PC sob tradutor x86->ARM. O loop principal do PC e'
 * while(PeekMessage){if WM_QUIT break} else run_frame(). fw_PeekMessageA estava
 * devolvendo WM_QUIT apos 1 frame -> jogo saia. Stubamos p/ "sem mensagem" (0):
 * o loop nunca ve WM_QUIT e roda frames pra sempre (input via DirectInput). */
static int ff7_peek_stub(void) { return 0; }
/* diagnostico: quem dispara o exit do loop Win32 do FF7 PC? */
static void ff7_exitproc_stub(unsigned code) { debugPrintf(">>> fw_ExitProcess(%u) chamado (no-op)\n", code); }
static void ff7_postquit_stub(unsigned code) { debugPrintf(">>> fw_PostQuitMessage(%u) chamado (no-op)\n", code); }
static unsigned ff7_destroywin_stub(unsigned h) { debugPrintf(">>> fw_DestroyWindow(%u) chamado (no-op)\n", h); return 1; }

/* pump de input (single-thread: chamado a cada present, nao ha loop SDL). */
static void ff7_pump_input(void) {
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    switch (e.type) {
      case SDL_QUIT: g_quit = 1; break;
      case SDL_KEYDOWN: case SDL_KEYUP:
        if (e.key.repeat) break;
        if (e.key.keysym.sym == SDLK_ESCAPE && e.type == SDL_KEYDOWN) { g_quit = 1; break; }
        if (onKey) { int kc = map_key_android(e.key.keysym.sym);
          if (kc >= 0) onKey(g_env, NULL, kc, e.type == SDL_KEYDOWN); }
        break;
      case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP: {
        int down = (e.type == SDL_CONTROLLERBUTTONDOWN);
        /* HOTKEY PADRAO PORTMASTER: SELECT+START juntos = sair do jogo. */
        static int hk_start = 0, hk_select = 0;
        if (e.cbutton.button == SDL_CONTROLLER_BUTTON_START) hk_start = down;
        if (e.cbutton.button == SDL_CONTROLLER_BUTTON_BACK)  hk_select = down;
        if (hk_start && hk_select) { debugPrintf("SELECT+START -> quit\n"); g_quit = 1; break; }
        /* SKIP do FMV REMOVIDO (era feature NOSSA, nao existe no FF7 original):
         * o script de campo SINCRONIZA com o filme (MVIEF espera frames do
         * video) — qualquer forma de pular (fw_stop_movie, force_eof, jump do
         * contador p/ 1791) deixa o script em limbo => campo sem musica/audio
         * (relato do usuario 2026-07-02). Comportamento nativo = video sempre
         * toca. FF7_FMVSKIP_BTN reativa p/ estudo (quebra o audio). */
        if (getenv("FF7_FMVSKIP_BTN") && down
            && e.cbutton.button == SDL_CONTROLLER_BUTTON_START && !hk_select) {
          const char *mn = fmv_current_name();
          if (fmv_has_movie() && mn && !strstr(mn, "logo") && !fmv_eof()) {
            extern void fmv_force_eof(void);
            if (text_base && fmv_total_frames() > 0)
              *(unsigned *)((uintptr_t)text_base + 0x1cd8c9c) = (unsigned)fmv_total_frames() - 1;
            fmv_force_eof();
            debugPrintf("FMV: SKIP (START, opt-in dev) -> jump contador + eof\n");
            break;
          }
        }
        send_button(e.cbutton.button, down);
        break; }
      case SDL_CONTROLLERAXISMOTION: {
        /* gatilhos L2/R2 (codigos 12/13) e analogico esquerdo -> dpad (0/1/2/3).
         * estado anterior p/ enviar so' na borda (nao spammar onKey). */
        static int tl = 0, tr = 0, ax = 0, ay = 0;  /* -1/0/1 p/ stick */
        int a = e.caxis.axis, v = e.caxis.value;
        if (a == SDL_CONTROLLER_AXIS_TRIGGERLEFT) {
          int on = v > 8000; if (on != tl) { tl = on; if (onKey) onKey(g_env, NULL, 12, on); }
        } else if (a == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
          int on = v > 8000; if (on != tr) { tr = on; if (onKey) onKey(g_env, NULL, 13, on); }
        } else if (a == SDL_CONTROLLER_AXIS_LEFTX) {
          /* deadzone ALTO (24000=~73%) p/ drift do analogico nao brigar c/ o dpad. */
          int d = v < -24000 ? -1 : v > 24000 ? 1 : 0;
          if (d != ax) {
            if (ax == -1 && onKey) onKey(g_env, NULL, 1, 0);  /* LEFT up */
            if (ax == 1 && onKey)  onKey(g_env, NULL, 3, 0);  /* RIGHT up */
            if (d == -1 && onKey) onKey(g_env, NULL, 1, 1);
            if (d == 1 && onKey)  onKey(g_env, NULL, 3, 1);
            ax = d;
          }
        } else if (a == SDL_CONTROLLER_AXIS_LEFTY) {
          int d = v < -24000 ? -1 : v > 24000 ? 1 : 0;
          if (d != ay) {
            if (ay == -1 && onKey) onKey(g_env, NULL, 0, 0);  /* UP up */
            if (ay == 1 && onKey)  onKey(g_env, NULL, 2, 0);  /* DOWN up */
            if (d == -1 && onKey) onKey(g_env, NULL, 0, 1);
            if (d == 1 && onKey)  onKey(g_env, NULL, 2, 1);
            ay = d;
          }
        }
        break;
      }
      case SDL_MOUSEBUTTONDOWN: if (onTouchBegan) onTouchBegan(g_env, NULL, e.button.x, e.button.y); break;
      case SDL_MOUSEBUTTONUP:   if (onTouchEnded) onTouchEnded(g_env, NULL, e.button.x, e.button.y); break;
      case SDL_MOUSEMOTION:
        if ((e.motion.state & SDL_BUTTON_LMASK) && onTouchMoved)
          onTouchMoved(g_env, NULL, e.motion.x, e.motion.y); break;
    }
  }
}

/* present callback: chamado pelo wrapper ff7_sem_post(semB) (frame renderizado,
 * context ja liberado pelo VIDEO_update). Faz o swap da janela + input. */
static void ff7_present_cb(void) {
  g_present_frame_n = g_frame; /* p/ o cap de upload do FMV (1x/frame) */
  /* DIAG: estado do movie/FMV (movie_object @0x1cd8c8c, counter @0x1cd8c9c). */
  if ((g_frame < 6 || g_frame % 120 == 0) && text_base) {
    uintptr_t mo = *(uintptr_t *)((uintptr_t)text_base + 0x1cd8c8c);
    unsigned mc = *(unsigned *)((uintptr_t)text_base + 0x1cd8c9c);
    uintptr_t pmo = *(uintptr_t *)((uintptr_t)text_base + 0x1cd8c90);
    debugPrintf("MOVIEDIAG frame %ld: movie_object=0x%lx pMovie=0x%lx frame_counter=%u\n",
                g_frame, (unsigned long)mo, (unsigned long)pmo, mc);
  }
  /* FF7_FMVSKIP: encerra o FMV de abertura (longo, preto sem decode) p/ chegar no
   * campo rapido. fw_stop_movie quando o movie_frame_counter indica movie tocando
   * (>4 evita boot). Gate por frame > 5 evita lixo de boot. */
  if (getenv("FF7_FMVSKIP") && g_fw_stop_movie && text_base && g_frame > 5) {
    unsigned mc = *(unsigned *)((uintptr_t)text_base + 0x1cd8c9c);
    if (mc > 4 && mc < 0x40000000u) { g_fw_stop_movie(); }
  }
  /* FF7_FMVSTOPFRAME=N: para o FMV so' DEPOIS do frame N (testar se o FMV preto
   * fica desenhado POR CIMA do campo, escondendo o background pre-renderizado). */
  if (g_fw_stop_movie && text_base) {
    const char *sf = getenv("FF7_FMVSTOPFRAME");
    if (sf && g_frame > atol(sf)) g_fw_stop_movie();
  }
  /* VIDEO_update LIBEROU o context (eglMakeCurrent NULL) antes do sem_post(semB).
   * Re-adquirimos o context p/ o swap (e o glReadPixels do shot) funcionar — sem
   * context, SDL_GL_SwapWindow vira no-op no Mali fbdev (= tela preta). Soltamos
   * de novo no fim p/ o eglMakeCurrent(rebind) do VIDEO_update funcionar. */
  /* com keepctx (default) o context NUNCA e' solto pelo engine -> ja' esta'
   * current, so' swappa (rapido). Sem keepctx, re-adquire (modo antigo). */
  extern int g_ff7_keepctx;
  int reacq = !g_ff7_keepctx && getenv("FF7_NOREACQ") == NULL;
  if (reacq && g_egl_dpy) eglMakeCurrent(g_egl_dpy, g_egl_surf, g_egl_surf, g_egl_ctx);
  /* OVERLAY do FMV: desenha o frame decodificado por cima do quad preto do engine,
   * ANTES do swap (o engine toca o filme intocado -> transicao limpa pro campo). */
  fmv_overlay_draw();
  if (g_frame < 3 || (g_shots_every > 0 && g_frame % g_shots_every == 0))
    ff7_dump_shot(g_w, g_h, (int)g_frame);
  gl_swap_guarded(g_window);
  if (reacq && g_egl_dpy) eglMakeCurrent(g_egl_dpy, NULL, NULL, NULL);
  /* AUDIO STREAMING: no modo single-thread (default) o loop do jogo roda DENTRO
   * do worker_fn, entao o pump dos buffer-queue callbacks (que realimenta audio
   * em streaming: musica/BGM e SFX em loop das batalhas) NAO era chamado em lugar
   * nenhum — so' o FF7_THREADED o chamava. Sem isso, so' SFX one-shot (pegar item,
   * selecionar) tocam (enfileirados 1x), e musica/audio de batalha ficam MUDOS.
   * Chamamos por frame aqui = realimenta o ring de cada player streaming. */
  /* tick do streaming de som ANTES do pump (decode Ogg -> QueueBuffer), senao a
   * BGM/audio de batalha drena = mudo. O thread-timer do engine nao dispara aqui. */
  if (g_SoundSystem_Update && getenv("FF7_NOSNDUPD") == NULL) {
    int sn = getenv("FF7_SNDUPD_N") ? atoi(getenv("FF7_SNDUPD_N")) : 1;
    if (sn < 1) sn = 1; if (sn > 8) sn = 8;
    for (int si = 0; si < sn; si++) g_SoundSystem_Update();
  }
  /* BGM-PCM: ativa+bombeia SO' apos o boot estabilizar (evita deadlock Mali). */
  if (getenv("FF7_FILLCB") && text_base && g_frame % 60 == 0) {
    unsigned char gate = *(unsigned char *)((char *)text_base + 0x1d1a000 + 2464);
    debugPrintf("FILLGATE frame %ld [+2464]=%u\n", g_frame, gate);
  }
  /* FIX TENTATIVO: o fill-cb da saida SQEX pula o render se [+2464]!=0 -> BGM muda.
   * Forca 0 (toca via player 0 do engine, sem player extra = sem deadlock Mali). */
  if (getenv("FF7_FILLGATE0") && text_base)
    *(unsigned char *)((char *)text_base + 0x1d1a000 + 2464) = 0;
  if (g_frame > 600) { ff7_bgm_maybe_activate(); ff7_bgm_pump(); }
  /* BGMTRACE: globals do SND (musica FF7-PC): handle da musica corrente
   * [0x1cce000+2352], volume-config [3568], volume desejado [3576]. */
  if (getenv("FF7_BGMTRACE") && text_base && g_frame % 120 == 0) {
    unsigned hd = *(unsigned *)((uintptr_t)text_base + 0x1cce000 + 2352);
    float vcfg = *(float *)((uintptr_t)text_base + 0x1cce000 + 3568);
    float vdes = *(float *)((uintptr_t)text_base + 0x1cce000 + 3576);
    debugPrintf("SNDGLOB frame %ld handle=%u vcfg=%f vdesejado=%f\n", g_frame, hd, vcfg, vdes);
  }
  /* FIM LIMPO do FMV: quando nosso decode chega no fim, chama fw_stop_movie 1x p/
   * o engine ENCERRAR o filme de vez e o script do campo spawnar o Cloud/party.
   * (campo carrega + musica toca, mas sem o stop limpo o script de entrada nao
   * spawna os personagens — relato do NextOS: "cloud nao aparece"). */
  /* FIM DO FMV pelo CONTADOR DO ENGINE (0x1cd8c9c, avanca ~1/frame de forma
   * CONFIAVEL). O decode (cur_frame) ESTAGNA quando a injecao para de ser chamada
   * -> fmv_eof nunca disparava -> o engine "tocava" o filme p/ sempre (counter
   * passava do total) -> o script do campo ESPERAVA o filme acabar -> CONGELAVA
   * preto pos-trem. Quando o counter alcanca o total REAL do .ivf, forcamos o fim:
   * fmv_force_eof (para a injecao) + fw_stop_movie (encerra no engine) -> o campo
   * carrega de verdade (Cloud entra). Default ON; FF7_NOSTOP desliga. */
  if (g_ff7_fmv_inject && getenv("FF7_NOSTOP") == NULL && getenv("FF7_NOFMV") == NULL
      && g_fw_stop_movie && fmv_has_movie() && text_base) {
    static int movend = 0;
    const char *mn = fmv_current_name();
    int isreal = mn && !strstr(mn, "logo");
    int tot = fmv_total_frames();
    unsigned ec = *(unsigned *)((uintptr_t)text_base + 0x1cd8c9c);
    if (isreal && tot > 0 && ec < (unsigned)tot) movend = 0;   /* tocando -> rearma */
    if (isreal && tot > 0 && ec >= (unsigned)tot && !movend) { /* atingiu o fim real */
      extern void fmv_force_eof(void); fmv_force_eof();
      g_fw_stop_movie(); movend = 1;
      debugPrintf("FMV: engine counter %u >= total %d -> fw_stop_movie (campo deve carregar)\n", ec, tot);
    }
  }
  /* REDE DE SEGURANCA do modo overlay: se o engine passar MUITO do total sem
   * encerrar o filme sozinho (nao deveria acontecer sem o hijack), forca o fim
   * 1x p/ nao congelar preto esperando (bug antigo do modo inject). */
  if (!g_ff7_fmv_inject && getenv("FF7_NOFMV") == NULL && g_fw_stop_movie
      && fmv_has_movie() && text_base) {
    extern int g_ff7_movie_active;
    static int safed = 0;
    const char *mn = fmv_current_name();
    int isreal = mn && !strstr(mn, "logo");
    int tot = fmv_total_frames();
    unsigned ec = *(unsigned *)((uintptr_t)text_base + 0x1cd8c9c);
    unsigned rel = (g_ovl_have_base && ec >= g_ovl_base_mc && ec < 0x40000000u)
                   ? ec - g_ovl_base_mc : 0;
    if (!g_ff7_movie_active || (tot > 0 && rel < (unsigned)tot + 300)) safed = 0; /* rearma */
    if (g_ff7_movie_active && isreal && tot > 0 && rel >= (unsigned)tot + 300 && !safed) {
      extern void fmv_force_eof(void); fmv_force_eof();
      g_fw_stop_movie(); safed = 1;
      debugPrintf("FMVOVL: SAFETY rel=%u >> total=%d -> force eof + fw_stop_movie\n", rel, tot);
    }
  }
  if (opensles_shim_pump_callbacks && getenv("FF7_NOAUDIOPUMP") == NULL)
    opensles_shim_pump_callbacks();
  /* DIAG/FIX master volume da musica (SQEX). FF7_MVOLDIAG loga o valor; FF7_FORCEMVOL
   * forca 1.0 (teste: se a BGM aparecer, o master estava 0). */
  if (g_GetMasterVolume && getenv("FF7_MVOLDIAG") && (g_frame < 3 || g_frame % 120 == 0))
    debugPrintf("MVOLDIAG frame %ld master_volume=%f\n", g_frame, g_GetMasterVolume());
  if (g_SetMasterVolume && getenv("FF7_FORCEMVOL") && g_frame % 30 == 0)
    g_SetMasterVolume(1.0f, 0);
  /* FF7_SKIPTEST=N (dev): reproduz o skip do START no frame N do movie, p/
   * depurar o "pulei o video -> jogo sem audio". FF7_SKIPMODE: 0=como o botao
   * (force_eof+fw_stop_movie), 1=NATIVO (so' force_eof -> FRAME=0 -> engine
   * encerra sozinho pelo caminho normal). */
  if (getenv("FF7_SKIPTEST")) {
    static int skipped = 0;
    extern int g_ff7_movie_active;
    const char *smn = fmv_current_name();
    if (!skipped && g_ff7_movie_active && fmv_has_movie() && smn
        && !strstr(smn, "logo") && !fmv_eof()
        && fmv_cur_frame() >= atoi(getenv("FF7_SKIPTEST"))) {
      int mode = getenv("FF7_SKIPMODE") ? atoi(getenv("FF7_SKIPMODE")) : 2;
      /* modo 2 (NATIVO CORRETO): o script de campo (MVIEF) ESPERA o contador do
       * filme chegar no frame final — pula avancando o contador pro fim + eof;
       * o engine encerra sozinho (FRAME=0 -> AVI_draw final -> done=1). */
      if (mode == 2 && text_base && fmv_total_frames() > 0)
        *(unsigned *)((uintptr_t)text_base + 0x1cd8c9c) = (unsigned)fmv_total_frames() - 1;
      extern void fmv_force_eof(void); fmv_force_eof();
      if (mode == 0 && g_fw_stop_movie) g_fw_stop_movie();
      skipped = 1;
      debugPrintf("SKIPTEST: skip no frame %d (modo %d)\n", fmv_cur_frame(), mode);
    }
  }
  ff7_pump_input();
  /* o menu de titulo so' atualiza quando callUpdateTitlemenu seta o flag (a Java
   * side chamava isso por frame/touch); no single-thread chamamos nos. */
  if (callUpdateTitlemenu) callUpdateTitlemenu(g_env, NULL);
  /* FF7_AUTOSKIP=1: sequencia automatica p/ TESTE (so dev). Fase 1: BUT_A repetido
   * skipa logo/creditos ate o menu. Fase 2 (dica do NextOS): no menu o cursor cai
   * em "Continue?"; UP 1x sobe p/ NEW GAME, A entra. Threshold ajustavel via
   * FF7_SKIP_MENUFRAME (frame em que o menu ja' esta' pronto, default 520). */
  /* idle so' em FMV "de verdade" (ex opening), NAO nos logos de boot (eidoslogo/
   * sqlogo) — esses a gente deixa o autoskip passar p/ chegar no New Game. */
  const char *mn = fmv_current_name();
  /* LATCH determinístico: assim que o filme de abertura ("opening", nao-logo) e'
   * REQUISITADO pelo campo (AVI_open), JA' passamos do menu (New Game entrou + campo
   * md1stin carregou). Desliga o autoskip PARA SEMPRE -> FMV/campo LIMPOS p/ observar.
   * Independe de START (vale mesmo com FF7_FMVSTART=0, onde o filme nem toca). */
  int real_movie = fmv_has_movie() && mn && !strstr(mn, "logo");
  int real_fmv = real_movie && !fmv_eof() && getenv("FF7_NOFMV") == NULL;
  /* LATCH p/ parar o autoskip assim que NEW GAME entra de fato: o movie de abertura
   * ENGATA (movie_frame_counter@0x1cd8c9c > 0) ou um filme real abre. Para ANTES de
   * o autoskip poder cancelar/sujar o jogo. */
  unsigned mvc = 0;
  if (text_base) mvc = *(unsigned *)((uintptr_t)text_base + 0x1cd8c9c);
  static int g_skip_done = 0;
  if (real_movie || (mvc > 0 && mvc < 0x40000000u)) g_skip_done = 1;
  if (getenv("FF7_AUTOSKIP") && onKey && !g_skip_done && g_frame > 30) {
    /* B (escapa tela de saves/Load) -> UP martelado (parka cursor no topo=NEW GAME)
     * -> A (entra). O LATCH (real_movie/counter>0) desliga tudo assim que New Game
     * entra, ANTES do proximo B poder cancelar. UP martelado evita cair em Continue;
     * o B so' serve de rede se cair na tela de Load. */
    long c = g_frame % 110;
    if (c == 0)  onKey(g_env, NULL, 5, 1);   /* B press (escapa Load) */
    if (c == 6)  onKey(g_env, NULL, 5, 0);
    if (c >= 18 && c < 84) { if (c % 10 == 8) onKey(g_env, NULL, 0, 1);    /* UP press */
                             if (c % 10 == 3) onKey(g_env, NULL, 0, 0); }  /* UP release */
    if (c == 96) onKey(g_env, NULL, 4, 1);   /* A press (entra NEW GAME) */
    if (c == 102) onKey(g_env, NULL, 4, 0);
  }
  /* FF7_AUTOADV (so' p/ VERIFICAR jogabilidade): DEPOIS do New Game e quando o FMV
   * NAO esta' tocando, martela A p/ avancar dialogos/cutscene/entrar em batalha.
   * Sem B/UP (nao cancela nada). Prova que pos-FMV continua jogavel ate a batalha. */
  if (getenv("FF7_AUTOADV") && onKey && g_skip_done && !real_fmv) {
    long c = g_frame % 50;
    if (c == 0) onKey(g_env, NULL, 4, 1);
    if (c == 8) onKey(g_env, NULL, 4, 0);
  }
  if (g_frame < 5 || g_frame % 120 == 0) debugPrintf("present frame %ld\n", g_frame);
  g_frame++;
  if (g_quit || (g_maxframes && g_frame >= g_maxframes)) {
    debugPrintf("present: exit (frame %ld)\n", g_frame);
    if (onPause) onPause(g_env, NULL);
    SDL_Quit();
    _exit(0);
  }
}

int main(int argc, char *argv[]) {
  { volatile char c = g_bionic_guard_pad[0]; (void)c; }
  {
    unsigned long tp; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    debugPrintf("TLSDIAG tp=0x%lx pad=%p tp+0x28=0x%lx pad_in_range=%d\n",
                tp, (void *)g_bionic_guard_pad, tp + 0x28,
                ((uintptr_t)g_bionic_guard_pad <= tp + 0x28 &&
                 tp + 0x28 < (uintptr_t)g_bionic_guard_pad + 256));
  }
  debugPrintf("=== FINAL FANTASY VII (materialg) AARCH64 so-loader ===\n");

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0)
    fatal_error("SDL_Init: %s", SDL_GetError());
  SDL_DisplayMode dm;
  if (SDL_GetDesktopDisplayMode(0, &dm) != 0)
    fatal_error("SDL_GetDesktopDisplayMode: %s", SDL_GetError());

  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

  SDL_Window *window = SDL_CreateWindow("FINAL FANTASY VII",
      SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, dm.w, dm.h,
      SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN);
  if (!window) fatal_error("SDL_CreateWindow: %s", SDL_GetError());
  SDL_GLContext glc = gl_create_context_guarded(window);
  if (!glc) fatal_error("SDL_GL_CreateContext: %s", SDL_GetError());
  int w, h; SDL_GL_GetDrawableSize(window, &w, &h);
  debugPrintf("Window %dx%d\n", w, h);

  open_gamepad();

  /* ---- libjni_ff7.so (engine + jogo). STL estatica -> sem modulo aux. ---- */
  size_t heap_size = (size_t)MEMORY_MB * 1024 * 1024;
  void *heap = mmap(NULL, heap_size, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (heap == MAP_FAILED) fatal_error("mmap heap %d MB", MEMORY_MB);

  if (so_load(SO_NAME, heap, heap_size) < 0) fatal_error("so_load %s", SO_NAME);
  debugPrintf("Loaded %s: text=%p+%zu data=%p+%zu\n", SO_NAME, text_base, text_size, data_base, data_size);
  if (so_relocate() < 0) fatal_error("so_relocate");
  if (so_resolve(dynlib_functions, dynlib_functions_count, 0) < 0) fatal_error("so_resolve");
  so_make_text_writable();
  so_flush_caches();
  so_execute_init_array();

  JNI_OnLoad       = (void *)so_find_addr("JNI_OnLoad");
  setAssetManager  = (void *)so_find_addr("Java_jp_co_d4e_materialg_GLESJniWrapper_setAssetManager");
  setDataPath      = (void *)so_find_addr("Java_jp_co_d4e_materialg_GLESJniWrapper_setDataPath");
  setLang          = (void *)so_find_addr("Java_jp_co_d4e_materialg_GLESJniWrapper_setLang");
  onSurfaceCreated = (void *)so_find_addr("Java_jp_co_d4e_materialg_GLESJniWrapper_onSurfaceCreated");
  onSurfaceChanged = (void *)so_find_addr("Java_jp_co_d4e_materialg_GLESJniWrapper_onSurfaceChanged");
  onDrawFrame      = (void *)so_find_addr("Java_jp_co_d4e_materialg_GLESJniWrapper_onDrawFrame");
  onResume         = (void *)so_find_addr("Java_jp_co_d4e_materialg_GLESJniWrapper_onResume");
  onPause          = (void *)so_find_addr("Java_jp_co_d4e_materialg_GLESJniWrapper_onPause");
  onKey            = (void *)so_find_addr("Java_jp_co_d4e_materialg_GLESJniWrapper_onKey");
  onKeyBack        = (void *)so_find_addr("Java_jp_co_d4e_materialg_GLESJniWrapper_onKeyBack");
  onTouchBegan     = (void *)so_find_addr("Java_jp_co_d4e_materialg_GLESJniWrapper_onTouchBegan");
  onTouchMoved     = (void *)so_find_addr("Java_jp_co_d4e_materialg_GLESJniWrapper_onTouchMoved");
  onTouchEnded     = (void *)so_find_addr("Java_jp_co_d4e_materialg_GLESJniWrapper_onTouchEnded");
  callUpdateTitlemenu = (void *)so_find_addr("Java_jp_co_d4e_materialg_GLESJniWrapper_callUpdateTitlemenu");
  g_fw_stop_movie = (void *)so_find_addr_safe("_Z13fw_stop_moviev");
  g_SoundSystem_Update = (void *)so_find_addr_safe("SoundSystem_Update");
  g_GetMasterVolume = (void *)so_find_addr_safe("_ZN4SQEX10CoreSystem15GetMasterVolumeEv");
  g_SetMasterVolume = (void *)so_find_addr_safe("_ZN4SQEX10CoreSystem15SetMasterVolumeEfj");
  /* FMV: default = OVERLAY desacoplado (engine toca o filme intocado e transiciona
   * limpo; nos desenhamos o video por cima no present cb). FF7_FMVINJECT volta ao
   * modo antigo (hijack do enableOES — corrompia o fim do filme = tela preta).
   * FF7_NOFMV desliga tudo (filme vira skip instantaneo, START=1). */
  g_ff7_fmv_inject = getenv("FF7_FMVINJECT") != NULL;
  if (getenv("FF7_NOFMV") == NULL) {
    if (g_ff7_fmv_inject) {
      uintptr_t ve = so_find_addr_safe("VIDEO_enableOES");
      if (ve) {
        g_orig_enableOES = (enableoes_fn)make_trampoline(ve, 16);
        if (g_orig_enableOES) { hook_arm64(ve, (uintptr_t)&my_enableOES);
          debugPrintf("FMV: hooked VIDEO_enableOES @%p tramp=%p (INJECT)\n", (void*)ve, (void*)g_orig_enableOES); }
      } else debugPrintf("FMV: VIDEO_enableOES nao achado\n");
    }
    uintptr_t ao = so_find_addr_safe("_Z8AVI_openPKc");
    if (ao) {
      g_orig_avi_open = (avi_open_fn)make_trampoline(ao, 16);
      if (g_orig_avi_open) { hook_arm64(ao, (uintptr_t)&my_avi_open);
        debugPrintf("FMV: hooked AVI_open @%p (overlay=%d)\n", (void*)ao, !g_ff7_fmv_inject); }
    } else debugPrintf("FMV: AVI_open nao achado\n");
  }
  /* FF7_BORDER=r,g,b[,r2,g2,b2]: troca a cor do bezel (default = azul do engine). */
  if (getenv("FF7_BORDER")) {
    int n = sscanf(getenv("FF7_BORDER"), "%d,%d,%d,%d,%d,%d",
                   &g_border_c[0], &g_border_c[1], &g_border_c[2],
                   &g_border_c[3], &g_border_c[4], &g_border_c[5]);
    if (n >= 3) {
      if (n < 6) { g_border_c[3] = g_border_c[0]; g_border_c[4] = g_border_c[1]; g_border_c[5] = g_border_c[2]; }
      uintptr_t bo = so_find_addr_safe("VIDEO_doQuad_color_forBorder");
      if (bo) {
        g_orig_border = (border_fn)make_trampoline(bo, 16);
        if (g_orig_border) { hook_arm64(bo, (uintptr_t)&my_border);
          debugPrintf("BORDER: hooked (%d,%d,%d)-(%d,%d,%d)\n",
                      g_border_c[0], g_border_c[1], g_border_c[2],
                      g_border_c[3], g_border_c[4], g_border_c[5]); }
      } else debugPrintf("BORDER: simbolo nao achado\n");
    }
  }
  /* SQEX @44100 (mata o resample duplo da BGM = chiado). Prologo de
   * CoreSystem::Initialize verificado relocavel (sub/cmp/stp/stp). */
  {
    uintptr_t ci = so_find_addr_safe("_ZN4SQEX10CoreSystem10InitializeEii");
    if (ci) { g_orig_coreinit = (coreinit_fn)make_trampoline(ci, 16);
      if (g_orig_coreinit) { hook_arm64(ci, (uintptr_t)&my_CoreInit);
        debugPrintf("COREINIT: hooked CoreSystem::Initialize @%p\n", (void*)ci); } }
  }
  /* NAO hookear SoundManager::Update: prologo tem adrp (PC-relative) na 3a
   * instrucao -> trampoline de 16B corrompe -> mutex em endereco lixo -> trava. */
  if (getenv("FF7_BGMTRACE")) {
    uintptr_t sv = so_find_addr_safe("_ZN4SQEX10CoreSource9SetVolumeEf");
    if (sv) { g_orig_setvol = (setvol_fn)make_trampoline(sv, 16);
      if (g_orig_setvol) hook_arm64(sv, (uintptr_t)&my_SetVolume); }
    uintptr_t sm2 = so_find_addr_safe("_ZN4SQEX10CoreSource15SetVolumeMatrixEff");
    if (sm2) { g_orig_setvolmx = (setvolmx_fn)make_trampoline(sm2, 16);
      if (g_orig_setvolmx) hook_arm64(sm2, (uintptr_t)&my_SetVolumeMatrix); }
    uintptr_t sp2 = so_find_addr_safe("_ZN4SQEX10CoreSource8SetPitchEf");
    if (sp2) { g_orig_setpitch = (setvol_fn)make_trampoline(sp2, 16);
      if (g_orig_setpitch) hook_arm64(sp2, (uintptr_t)&my_SetPitch); }
    uintptr_t lp = so_find_addr_safe("_ZN4SQEX10CoreSource9SetIIRLPFEf");
    if (lp) { g_orig_setlpf = (setvol_fn)make_trampoline(lp, 16);
      if (g_orig_setlpf) hook_arm64(lp, (uintptr_t)&my_SetIIRLPF); }
    uintptr_t pn = so_find_addr_safe("_ZN4SQEX10CoreSource6SetPanEf");
    if (pn) { g_orig_setpan = (setvol_fn)make_trampoline(pn, 16);
      if (g_orig_setpan) hook_arm64(pn, (uintptr_t)&my_SetPan); }
    uintptr_t st2 = so_find_addr_safe("_ZN4SQEX10CoreSource4StopEv");
    if (st2) { g_orig_srcstop = (srcstop_fn)make_trampoline(st2, 16);
      if (g_orig_srcstop) hook_arm64(st2, (uintptr_t)&my_SourceStop); }
    uintptr_t sa = so_find_addr_safe("_ZN4SQEX10CoreSource5StartEv");
    if (sa) { g_orig_srcstart = (srcstop_fn)make_trampoline(sa, 16);
      if (g_orig_srcstart) hook_arm64(sa, (uintptr_t)&my_SourceStart); }
    uintptr_t sd = so_find_addr_safe("SdSoundSystem_SoundCtrl_SetVolume");
    if (sd) { g_orig_sdvol = (sdvol_fn)make_trampoline(sd, 16);
      if (g_orig_sdvol) hook_arm64(sd, (uintptr_t)&my_SdVolume); }
    debugPrintf("BGMTRACE: SetVolume/Matrix/Pitch/LPF/Pan/Start/Stop/SdVol hookados\n");
  }
  if (getenv("FF7_QBUFLOG")) {
    uintptr_t qb = so_find_addr_safe("_ZN4SQEX10CoreSource11QueueBufferEPvm");
    if (qb) { g_orig_queuebuf = (queuebuf_fn)make_trampoline(qb, 16);
      if (g_orig_queuebuf) { hook_arm64(qb, (uintptr_t)&my_QueueBuffer);
        debugPrintf("QBUFLOG: hooked CoreSource::QueueBuffer @%p\n", (void*)qb); } }
  }
  if (getenv("FF7_MIDILOG")) {
    uintptr_t pm = so_find_addr_safe("_Z12fw_play_midijjj");
    if (pm) { g_orig_playmidi = (playmidi_fn)make_trampoline(pm, 16);
      if (g_orig_playmidi) { hook_arm64(pm, (uintptr_t)&my_playmidi);
        debugPrintf("MIDILOG: hooked fw_play_midi @%p\n", (void*)pm); } }
  }

  /* TESTE de decode (sem GL): decodifica N frames do opening e grava PPM -> prova
   * o pipeline VP8->RGBA isolado. FF7_FMVDUMP=N (default 6). */
  if (getenv("FF7_FMVDUMP")) {
    int nf = atoi(getenv("FF7_FMVDUMP")); if (nf <= 0) nf = 6;
    fmv_set_movie_from_webm("/roms/ports/ff7/gamedata/ff7_1.02/data/movies/opening.webm");
    const char *home = getenv("HOME"); if (!home) home = "/tmp";
    for (int i = 0; i < nf; i++) {
      if (!fmv_next_frame()) { debugPrintf("FMVDUMP: sem frame %d\n", i); break; }
      int w = fmv_w(), h = fmv_h(); const unsigned char *r = fmv_rgba();
      char p[256]; snprintf(p, sizeof p, "%s/fmvdump_%02d.ppm", home, i);
      FILE *f = fopen(p, "wb");
      if (f) { fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int k = 0; k < w * h; k++) fwrite(r + (size_t)k * 4, 1, 3, f);
        fclose(f); debugPrintf("FMVDUMP wrote %s (%dx%d)\n", p, w, h); }
    }
    _exit(0);
  }

  /* Hook do RenderMix p/ o BYPASS de BGM (default ON; FF7_NOMUSICBYPASS desliga). */
  g_music_bypass = (getenv("FF7_NOMUSICBYPASS") == NULL);
  if (g_music_bypass || getenv("FF7_RMDIAG")) {
    uintptr_t rm = so_find_addr_safe("_ZN4SQEX10CoreSource9RenderMixEPvm");
    if (rm) {
      g_orig_rendermix = (rendermix_fn)make_trampoline(rm, 16);
      if (g_orig_rendermix) { hook_arm64(rm, (uintptr_t)&my_RenderMix);
        debugPrintf("BGM bypass: hooked RenderMix @%p tramp=%p bypass=%d\n",
                    (void*)rm, (void*)g_orig_rendermix, g_music_bypass); }
      else g_music_bypass = 0;
    } else g_music_bypass = 0;
  }
  setBatteryLevel  = (void *)so_find_addr("Java_jp_co_d4e_materialg_GLESJniWrapper_setBatteryLevel");
  VIDEO_update     = (void *)so_find_addr_safe("VIDEO_update");

  /* stub fw_PeekMessageA -> 0 (senao o loop Win32 do FF7 PC ve WM_QUIT e sai). */
  {
    uintptr_t peek = so_find_addr_safe("_Z15fw_PeekMessageAjjjjj");
    if (peek) { hook_arm64(peek, (uintptr_t)&ff7_peek_stub);
      debugPrintf("hooked fw_PeekMessageA -> 0\n"); }
    else debugPrintf("WARN: fw_PeekMessageA nao achado\n");
    uintptr_t ep = so_find_addr_safe("_Z14fw_ExitProcessj");
    if (ep) hook_arm64(ep, (uintptr_t)&ff7_exitproc_stub);
    uintptr_t pq = so_find_addr_safe("_Z18fw_PostQuitMessagej");
    if (pq) hook_arm64(pq, (uintptr_t)&ff7_postquit_stub);
    uintptr_t dw = so_find_addr_safe("_Z16fw_DestroyWindowj");
    if (dw) hook_arm64(dw, (uintptr_t)&ff7_destroywin_stub);
    debugPrintf("exit-diag hooks: ExitProcess=%p PostQuit=%p DestroyWin=%p\n",
                (void*)ep, (void*)pq, (void*)dw);
  }

  if (!onDrawFrame || !onSurfaceChanged)
    fatal_error("missing GLESJniWrapper onDrawFrame/onSurfaceChanged");

  void *fake_vm = NULL, *fake_env = NULL;
  jni_shim_init(&fake_vm, &fake_env);
  g_env = fake_env;

  debugPrintf("JNI_OnLoad...\n");
  if (JNI_OnLoad) JNI_OnLoad(fake_vm, NULL);

  void *dummy = (void *)0x1337;
  /* setAssetManager: AAssetManager_fromJava devolve fake; AAsset shim le ./assets/ */
  if (setAssetManager) { debugPrintf("setAssetManager\n"); setAssetManager(fake_env, NULL, dummy); }

  /* setDataPath: dir contendo ff7_1.02/. OBB extraido (sem prefixo assets/). */
  const char *datapath = getenv("FF7_DATA");
  if (!datapath) datapath = "/storage/roms/ports/ff7/data";
  void *jpath = jni_make_string(datapath);
  if (setDataPath) { debugPrintf("setDataPath(%s)\n", datapath); setDataPath(fake_env, NULL, jpath); }

  /* JAMAIS JAPONES: forcar ingles. setLang: 0=EN 1=FR 2=DE 3/4=ES/JP (verif. por
   * screenshot 2026-06-24). FF7_LANG default 0 = INGLES. */
  int lang = getenv("FF7_LANG") ? atoi(getenv("FF7_LANG")) : 0;
  if (setLang) { debugPrintf("setLang(%d)\n", lang); setLang(fake_env, NULL, lang); }

  if (setBatteryLevel) setBatteryLevel(fake_env, NULL, 100);

  debugPrintf("onSurfaceCreated...\n");
  if (onSurfaceCreated) onSurfaceCreated(fake_env, NULL);
  debugPrintf("onSurfaceChanged(%d,%d)...\n", w, h);
  onSurfaceChanged(fake_env, NULL, w, h);
  if (onResume) onResume(fake_env, NULL);

  g_window = window; g_w = w; g_h = h;
  g_maxframes = getenv("FF7_MAXFRAMES") ? atol(getenv("FF7_MAXFRAMES")) : 0;
  g_shots_every = getenv("FF7_SHOTS") ? atol(getenv("FF7_SHOTS")) : 0;

  /* ==== SINGLE-THREAD (default) ====
   * O FF7 roda o jogo (main_ff7) num WORKER thread com o EGL context migrado
   * via eglMakeCurrent — o que TRAVA no Mali fbdev (surface presa a' thread que
   * a criou). Em vez disso rodamos main_ff7 na PROPRIA main thread (dona do
   * context). A funcao do worker (libjni+0x150658) faz JNI_attachMe +
   * eglMakeCurrent(rebind, mesma thread=OK) + main_ff7(loop do jogo).
   * VIDEO_update, a cada frame, faz sem_post(semB)/sem_wait(semA) p/ entregar o
   * present a' "UI thread"; nossos wrappers (imports.c) fazem o swap no
   * sem_post(semB) via g_ff7_present_cb e nao bloqueiam no sem_wait(semA).
   * struct de present @ vaddr 0x1cce0b0: +0 semA, +0x10 semB, +32 dpy, +40/48
   * surf, +56 ctx, +64 tid-dona. Populamos os handles EGL e tid = main. */
  if (getenv("FF7_THREADED")) {
    /* fallback: deixa ff7_DrawFrame fazer o threaded (so renderiza o bezel). */
    debugPrintf("render mode: ff7_DrawFrame (threaded, fallback)\n");
    while (!g_quit) {
      ff7_pump_input();
      if (callUpdateTitlemenu) callUpdateTitlemenu(g_env, NULL);
      if (opensles_shim_pump_callbacks) opensles_shim_pump_callbacks();
      onDrawFrame(g_env, NULL);
      gl_swap_guarded(window);
      if (g_shots_every>0 && g_frame>0 && g_frame%g_shots_every==0) ff7_dump_shot(w,h,(int)g_frame);
      g_frame++;
      if (g_maxframes && g_frame >= g_maxframes) break;
    }
  } else {
    uintptr_t base = (uintptr_t)text_base;
    uintptr_t st = base + 0x1cce0b0;            /* struct de present */
    *(void **)(st + 32) = eglGetCurrentDisplay();
    *(void **)(st + 40) = eglGetCurrentSurface(0x3059 /*EGL_DRAW*/);
    *(void **)(st + 48) = eglGetCurrentSurface(0x305a /*EGL_READ*/);
    *(void **)(st + 56) = eglGetCurrentContext();
    g_egl_dpy = *(void **)(st + 32); g_egl_surf = *(void **)(st + 40);
    g_egl_ctx = *(void **)(st + 56);  /* p/ re-bind no present cb */
    *(void **)(st + 64) = (void *)pthread_self();  /* tid dona do present = main */
    g_ff7_present_semA = (void *)(st + 0);
    g_ff7_present_semB = (void *)(st + 0x10);
    g_ff7_present_cb = ff7_present_cb;
    /* PERF: manter o context sempre current (engine nao solta) -> swap rapido. */
    extern int g_ff7_keepctx;
    if (!getenv("FF7_RELEASECTX")) g_ff7_keepctx = 1;
    debugPrintf("present struct: dpy=%p surf=%p ctx=%p tid=%p semA=%p semB=%p\n",
                *(void **)(st+32), *(void **)(st+40), *(void **)(st+56),
                *(void **)(st+64), g_ff7_present_semA, g_ff7_present_semB);

    /* arg do worker: [+0]=global(0x133a000+2408), [+8]=JNIEnv */
    void *(*JNI_getEnv)(void) = (void *)so_find_addr_safe("JNI_getEnv");
    void *worker_arg[4];
    worker_arg[0] = *(void **)(base + 0x133a000 + 2408);
    worker_arg[1] = JNI_getEnv ? JNI_getEnv() : g_env;
    worker_arg[2] = NULL; worker_arg[3] = NULL;

    void (*worker_fn)(void *) = (void *)(base + 0x150658);
    int loop_drive = getenv("FF7_LOOPDRIVE") != NULL;
    debugPrintf("render mode: single-thread%s; worker_fn=%p\n",
                loop_drive ? " (loop-drive)" : "", (void *)worker_fn);
    if (loop_drive) {
      /* main_ff7 faz 1 frame por chamada (design mobile: driver externo).
       * Chamamos em loop -> anima. (init guardado na 1a chamada.) */
      int n = 0;
      while (!g_quit && (!g_maxframes || g_frame < g_maxframes)) {
        worker_fn(worker_arg);
        if (++n <= 3 || n % 60 == 0) debugPrintf("worker_fn iter %d (frame %ld)\n", n, g_frame);
      }
    } else {
      worker_fn(worker_arg);   /* uma vez (main_ff7 deveria ser o loop) */
    }
    debugPrintf("worker_fn returned (game exited)\n");
  }

  debugPrintf("Exiting...\n");
  if (onPause) onPause(g_env, NULL);
  if (g_gamepad) SDL_GameControllerClose(g_gamepad);
  SDL_Quit();
  return 0;
}
