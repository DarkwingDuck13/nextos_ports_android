#define _GNU_SOURCE
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "jni_shim.h"
#include "util.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#define JNI_VTABLE_SIZE 512
#define MAX_JSTRINGS 256
#define MAX_ARRAYS 128

typedef int jint;
typedef unsigned char jboolean;

static uintptr_t jni_env_vtable[JNI_VTABLE_SIZE];
static void *jni_env_ptr;
static uintptr_t java_vm_vtable[JNI_VTABLE_SIZE];
static void *java_vm_ptr;

static int g_key_state;
static int g_key_trigger;
static int g_text_draw_serial;
static int g_movie_state = 5;
static char g_movie_name[256];
static int g_movie_duration_ms;
static long long g_movie_start_ms;

enum MethodTag {
  MID_UNKNOWN = 0,
  MID_GET_LANGUAGE,
  MID_GET_ANDROID_ID,
  MID_GET_APP_VERSION_NAME,
  MID_GET_MOUNTED_OBB_PATH,
  MID_IS_EXPANSION_FILE_NATIVE,
  MID_MOUNT_EXPANSION_FILE_NATIVE,
  MID_UNMOUNT_EXPANSION_FILE_NATIVE,
  MID_IS_MEDIA_MOUNTED,
  MID_GET_KEY_STATE,
  MID_GET_KEY_TRIGGER,
  MID_CLEAR_KEY_TRIGGER,
  MID_GET_FILE_DATA_JAVA,
  MID_GET_FONT_ASCENT,
  MID_GET_FONT_DESCENT,
  MID_GET_FONT_HEIGHT,
  MID_GET_FONT_WIDTH,
  MID_ON_TEXT_DRAW,
  MID_ON_TEST,
  MID_SET_MOVIE,
  MID_PLAY_MOVIE,
  MID_STOP_MOVIE,
  MID_IS_MOVIE_FINISHED,
  MID_IS_MOVIE_PLAYING,
  MID_GET_MOVIE_POSITION,
  MID_OPEN_URL,
  MID_TWEET,
  MID_LOGIN_TWITTER,
  MID_IS_TWITTER_TOKEN_ENABLE,
  MID_IS_TWITTER_CONNECTING,
  MID_CREATE_INDICATOR,
  MID_DELETE_INDICATOR,
  MID_IS_ACTIVE_INDICATOR,
  MID_START_INDICATOR,
  MID_STOP_INDICATOR,
  MID_CREATE_OK_DIALOG,
  MID_IS_DIALOG_VISIBLE,
  MID_GET_DIALOG_RETURN_VAL,
  MID_GENERIC
};

static int g_method_tags[128];

typedef struct {
  void *handle;
  char *value;
} FakeString;
static FakeString g_strings[MAX_JSTRINGS];
static int g_string_next;
static unsigned char g_string_handles[MAX_JSTRINGS];

typedef enum { ARR_NONE, ARR_BYTE, ARR_INT, ARR_FLOAT, ARR_OBJECT } ArrType;
typedef struct {
  void *handle;
  ArrType type;
  int len;
  void *data;
} FakeArray;
static FakeArray g_arrays[MAX_ARRAYS];
static int g_array_next;
static unsigned char g_array_handles[MAX_ARRAYS];
static int *g_text_pixels;
static void *g_text_pixels_array;
static unsigned char g_text_pixels_handle;
static FakeArray g_text_pixels_fake;
static unsigned char *g_font_data;
static stbtt_fontinfo g_font;
static int g_font_ready;

static intptr_t jni_stub(void) { return 0; }

static long long monotonic_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static int movie_duration_for_name(const char *name) {
  if (!name || !*name)
    return 90000;
  if (!strcmp(name, "kofi2012_s.m4v")) return 96367;
  if (!strcmp(name, "kofi2012_l.m4v")) return 151000;
  if (!strcmp(name, "kof13_PS3XBOX.m4v")) return 146542;
  if (!strcmp(name, "PV_S.m4v")) return 215567;
  if (!strcmp(name, "ac_openings.m4v")) return 61800;
  if (!strcmp(name, "ed_elisa.m4v")) return 84984;
  if (!strcmp(name, "ed_garou.m4v")) return 45579;
  if (!strcmp(name, "ed_ikari.m4v")) return 111979;
  if (!strcmp(name, "ed_japan.m4v")) return 49282;
  if (!strcmp(name, "ed_jyokaku.m4v")) return 97997;
  if (!strcmp(name, "ed_kim.m4v")) return 60494;
  if (!strcmp(name, "ed_k.m4v")) return 83984;
  if (!strcmp(name, "ed_psycho.m4v")) return 68267;
  if (!strcmp(name, "ed_ryuko.m4v")) return 98297;
  if (!strcmp(name, "ed_yagami.m4v")) return 54620;
  return 90000;
}

static void movie_update_finished(void) {
  if (g_movie_state == 2 && g_movie_duration_ms > 0) {
    long long pos = monotonic_ms() - g_movie_start_ms;
    if (pos >= g_movie_duration_ms)
      g_movie_state = 5;
  }
}

void kof_jni_key_down(int mask) {
  if ((g_key_state & mask) == 0)
    g_key_trigger |= mask;
  g_key_state |= mask;
}

void kof_jni_key_up(int mask) {
  g_key_state &= ~mask;
}

void kof_jni_set_key_state(int state) {
  int changed_on = state & ~g_key_state;
  g_key_trigger |= changed_on;
  g_key_state = state;
}

void kof_jni_clear_key_trigger(void) { g_key_trigger = 0; }
int kof_jni_key_state(void) { return g_key_state; }
int kof_jni_key_trigger(void) { return g_key_trigger; }
int kof_jni_text_draw_serial(void) { return g_text_draw_serial; }

const char *kof_jni_movie_name(void) { return g_movie_name; }
int kof_jni_movie_state(void) {
  movie_update_finished();
  return g_movie_state;
}
int kof_jni_movie_position_ms(void) {
  movie_update_finished();
  if (g_movie_state == 2) {
    long long pos = monotonic_ms() - g_movie_start_ms;
    if (pos < 0) pos = 0;
    if (g_movie_duration_ms > 0 && pos > g_movie_duration_ms)
      pos = g_movie_duration_ms;
    return (int)pos;
  }
  return g_movie_state == 5 ? g_movie_duration_ms : 0;
}
int kof_jni_movie_duration_ms(void) { return g_movie_duration_ms; }
int kof_jni_movie_is_playing(void) {
  movie_update_finished();
  return g_movie_state == 2;
}
void kof_jni_movie_mark_finished(void) { g_movie_state = 5; }

void *jni_make_string(const char *value) {
  int i = g_string_next++ % MAX_JSTRINGS;
  free(g_strings[i].value);
  g_strings[i].value = strdup(value ? value : "");
  g_strings[i].handle = &g_string_handles[i];
  return g_strings[i].handle;
}

static const char *string_value(void *jstr) {
  if (!jstr)
    return "";
  for (int i = 0; i < MAX_JSTRINGS; i++)
    if (g_strings[i].handle == jstr)
      return g_strings[i].value ? g_strings[i].value : "";
  return "";
}

static void *array_new(ArrType type, int len, const void *src) {
  if (len < 0)
    len = 0;
  int i = g_array_next++ % MAX_ARRAYS;
  free(g_arrays[i].data);
  g_arrays[i].data = NULL;
  g_arrays[i].type = type;
  g_arrays[i].len = len;
  g_arrays[i].handle = &g_array_handles[i];

  size_t elem = 1;
  if (type == ARR_INT || type == ARR_FLOAT)
    elem = 4;
  else if (type == ARR_OBJECT)
    elem = sizeof(void *);
  if (len > 0) {
    g_arrays[i].data = calloc((size_t)len, elem);
    if (src)
      memcpy(g_arrays[i].data, src, (size_t)len * elem);
  }
  return g_arrays[i].handle;
}

static FakeArray *array_find(void *handle) {
  if (!handle)
    return NULL;
  if (handle == &g_text_pixels_handle && g_text_pixels) {
    g_text_pixels_fake.handle = &g_text_pixels_handle;
    g_text_pixels_fake.type = ARR_INT;
    g_text_pixels_fake.len = 1024 * 1024;
    g_text_pixels_fake.data = g_text_pixels;
    return &g_text_pixels_fake;
  }
  for (int i = 0; i < MAX_ARRAYS; i++)
    if (g_arrays[i].handle == handle)
      return &g_arrays[i];
  return NULL;
}

void *kof_jni_int_array(const int *values, int len) {
  return array_new(ARR_INT, len, values);
}

void *kof_jni_float_array(const float *values, int len) {
  return array_new(ARR_FLOAT, len, values);
}

static void ensure_text_pixels(void) {
  if (!g_text_pixels) {
    g_text_pixels = calloc(1024 * 1024, sizeof(int));
    g_text_pixels_array = &g_text_pixels_handle;
  }
  g_text_pixels_fake.handle = &g_text_pixels_handle;
  g_text_pixels_fake.type = ARR_INT;
  g_text_pixels_fake.len = 1024 * 1024;
  g_text_pixels_fake.data = g_text_pixels;
}

static int *int_array_data(void *handle, int *len_out) {
  FakeArray *a = array_find(handle);
  if (!a || a->type != ARR_INT || !a->data) {
    if (len_out) *len_out = 0;
    return NULL;
  }
  if (len_out) *len_out = a->len;
  return (int *)a->data;
}

static void **object_array_data(void *handle, int *len_out) {
  FakeArray *a = array_find(handle);
  if (!a || a->type != ARR_OBJECT || !a->data) {
    if (len_out) *len_out = 0;
    return NULL;
  }
  if (len_out) *len_out = a->len;
  return (void **)a->data;
}

static int array_int_at(void *handle, int idx, int fallback) {
  int len = 0;
  int *p = int_array_data(handle, &len);
  if (!p || idx < 0 || idx >= len)
    return fallback;
  return p[idx];
}

static const char *string_array_at(void *handle, int idx) {
  int len = 0;
  void **p = object_array_data(handle, &len);
  if (!p || idx < 0 || idx >= len)
    return "";
  return string_value(p[idx]);
}

static unsigned char *read_file_all(const char *path, size_t *size_out) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n <= 0) {
    fclose(f);
    return NULL;
  }
  unsigned char *buf = malloc((size_t)n);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
    fclose(f);
    free(buf);
    return NULL;
  }
  fclose(f);
  if (size_out) *size_out = (size_t)n;
  return buf;
}

static int ensure_font(void) {
  if (g_font_ready > 0)
    return 1;
  if (g_font_ready < 0)
    return 0;

  const char *paths[] = {
      getenv("KOF_FONT_PATH"),
      "./kof_font.ttf",
      "./assets/kof_font.ttf",
      "/roms/ports/kof2012a/assets/kof_font.ttf",
      "/roms/ports/kof2012a/kof_font.ttf",
      "/usr/share/fonts/Adwaita/AdwaitaSans-Regular.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/gnu-free/FreeSans.otf",
  };

  for (int i = 0; i < (int)(sizeof(paths) / sizeof(paths[0])); i++) {
    if (!paths[i] || !*paths[i])
      continue;
    size_t size = 0;
    unsigned char *data = read_file_all(paths[i], &size);
    if (!data) {
      if (getenv("KOF_FONTLOG"))
        debugPrintf("[font] miss %s\n", paths[i]);
      continue;
    }
    int off = stbtt_GetFontOffsetForIndex(data, 0);
    int ok = off >= 0 && stbtt_InitFont(&g_font, data, off);
    if (getenv("KOF_FONTLOG")) {
      unsigned b0 = size > 0 ? data[0] : 0;
      unsigned b1 = size > 1 ? data[1] : 0;
      unsigned b2 = size > 2 ? data[2] : 0;
      unsigned b3 = size > 3 ? data[3] : 0;
      debugPrintf("[font] try %s size=%zu head=%02x%02x%02x%02x off=%d ok=%d\n",
                  paths[i], size, b0, b1, b2, b3, off, ok);
    }
    if (ok) {
      g_font_data = data;
      g_font_ready = 1;
      debugPrintf("[font] usando %s (%zu bytes)\n", paths[i], size);
      return 1;
    }
    free(data);
  }

  debugPrintf("[font] nenhuma TTF valida encontrada; usando metricas fallback\n");
  g_font_ready = -1;
  return 0;
}

static int font_ascent_px(int size) {
  if (size <= 0)
    size = 16;
  if (!ensure_font())
    return -(size * 3) / 4;
  int ascent, descent, linegap;
  stbtt_GetFontVMetrics(&g_font, &ascent, &descent, &linegap);
  float scale = stbtt_ScaleForPixelHeight(&g_font, (float)size);
  return -(int)ceilf((float)ascent * scale);
}

static int font_descent_px(int size) {
  if (size <= 0)
    size = 16;
  if (!ensure_font())
    return size / 4;
  int ascent, descent, linegap;
  stbtt_GetFontVMetrics(&g_font, &ascent, &descent, &linegap);
  float scale = stbtt_ScaleForPixelHeight(&g_font, (float)size);
  return (int)ceilf((float)(-descent) * scale);
}

static int utf8_next(const unsigned char **s) {
  const unsigned char *p = *s;
  if (!*p)
    return 0;
  if (*p < 0x80) {
    *s = p + 1;
    return *p;
  }
  if ((*p & 0xe0) == 0xc0 && p[1]) {
    *s = p + 2;
    return ((*p & 0x1f) << 6) | (p[1] & 0x3f);
  }
  if ((*p & 0xf0) == 0xe0 && p[1] && p[2]) {
    *s = p + 3;
    return ((*p & 0x0f) << 12) | ((p[1] & 0x3f) << 6) | (p[2] & 0x3f);
  }
  if ((*p & 0xf8) == 0xf0 && p[1] && p[2] && p[3]) {
    *s = p + 4;
    return ((*p & 0x07) << 18) | ((p[1] & 0x3f) << 12) |
           ((p[2] & 0x3f) << 6) | (p[3] & 0x3f);
  }
  *s = p + 1;
  return '?';
}

static int font_width_px(int size, const char *text) {
  if (size <= 0)
    size = 16;
  if (size > 96)
    size = 96;
  if (!text)
    text = "";
  if (!ensure_font())
    return (int)strlen(text) * (size > 0 ? size / 2 : 8);

  float scale = stbtt_ScaleForPixelHeight(&g_font, (float)size);
  float x = 0.0f;
  int prev = 0;
  const unsigned char *s = (const unsigned char *)text;
  int glyphs = 0;
  while (*s && glyphs++ < 256) {
    int cp = utf8_next(&s);
    if (cp == '\r' || cp == '\n')
      break;
    if (cp < 32)
      continue;
    if (cp != ' ' && !stbtt_FindGlyphIndex(&g_font, cp))
      cp = '?';
    int advance, lsb;
    stbtt_GetCodepointHMetrics(&g_font, cp, &advance, &lsb);
    if (prev)
      x += (float)stbtt_GetCodepointKernAdvance(&g_font, prev, cp) * scale;
    x += (float)advance * scale;
    prev = cp;
  }
  return (int)ceilf(x);
}

static int next_pow2_clamped(int value) {
  int p = 32;
  if (value < 1)
    value = 1;
  while (p < value && p < 1024)
    p <<= 1;
  if (p > 1024)
    p = 1024;
  return p;
}

static void blend_argb(int x, int y, int w, int h, int color, int coverage) {
  if (!g_text_pixels || x < 0 || y < 0 || x >= w || y >= h || coverage <= 0)
    return;

  int src_a = (color >> 24) & 255;
  if (src_a == 0)
    src_a = 255;
  src_a = (src_a * coverage) / 255;
  if (src_a <= 0)
    return;

  int src_r = (color >> 16) & 255;
  int src_g = (color >> 8) & 255;
  int src_b = color & 255;
  int *dstp = &g_text_pixels[y * w + x];
  int dst = *dstp;
  int dst_a = (dst >> 24) & 255;
  int dst_r = (dst >> 16) & 255;
  int dst_g = (dst >> 8) & 255;
  int dst_b = dst & 255;
  int out_a = src_a + (dst_a * (255 - src_a)) / 255;
  int out_r = (src_r * src_a + dst_r * dst_a * (255 - src_a) / 255) /
              (out_a ? out_a : 1);
  int out_g = (src_g * src_a + dst_g * dst_a * (255 - src_a) / 255) /
              (out_a ? out_a : 1);
  int out_b = (src_b * src_a + dst_b * dst_a * (255 - src_a) / 255) /
              (out_a ? out_a : 1);
  *dstp = (out_a << 24) | (out_r << 16) | (out_g << 8) | out_b;
}

static void draw_text_ttf(const char *text, float x, float baseline, int size,
                          int color, int tex_w, int tex_h) {
  if (!text || !*text || size <= 0 || !ensure_font())
    return;
  if (size > 96)
    size = 96;

  float scale = stbtt_ScaleForPixelHeight(&g_font, (float)size);
  float xpos = x;
  int prev = 0;
  const unsigned char *s = (const unsigned char *)text;
  int glyphs = 0;
  while (*s && glyphs++ < 256) {
    int cp = utf8_next(&s);
    if (cp == '\r' || cp == '\n')
      break;
    if (cp < 32)
      continue;
    if (cp != ' ' && !stbtt_FindGlyphIndex(&g_font, cp))
      cp = '?';
    int advance, lsb;
    stbtt_GetCodepointHMetrics(&g_font, cp, &advance, &lsb);
    if (prev)
      xpos += (float)stbtt_GetCodepointKernAdvance(&g_font, prev, cp) * scale;

    int x0, y0, x1, y1;
    stbtt_GetCodepointBitmapBox(&g_font, cp, scale, scale, &x0, &y0, &x1, &y1);
    int gw = x1 - x0;
    int gh = y1 - y0;
    if (gw > 0 && gh > 0 && gw <= 256 && gh <= 256) {
      unsigned char *glyph = malloc((size_t)gw * (size_t)gh);
      if (glyph) {
        stbtt_MakeCodepointBitmap(&g_font, glyph, gw, gh, gw, scale, scale, cp);
        int ox = (int)floorf(xpos) + x0;
        int oy = (int)floorf(baseline) + y0;
        for (int gy = 0; gy < gh; gy++) {
          int py = oy + gy;
          if (py < 0 || py >= tex_h)
            continue;
          for (int gx = 0; gx < gw; gx++) {
            int px = ox + gx;
            if (px < 0 || px >= tex_w)
              continue;
            blend_argb(px, py, tex_w, tex_h, color, glyph[gy * gw + gx]);
          }
        }
        free(glyph);
      }
    }
    xpos += (float)advance * scale;
    prev = cp;
  }
}

static void *draw_text_bitmap(void *info_arr, float scale, void *size_arr,
                              void *color_arr, void *x_arr, void *y_arr,
                              void *text_arr) {
  g_text_draw_serial++;
  ensure_text_pixels();
  memset(g_text_pixels, 0, (size_t)1024 * 1024 * sizeof(int));

  int count = array_int_at(info_arr, 0, 0);
  int logical_w = array_int_at(info_arr, 1, 480);
  int logical_h = array_int_at(info_arr, 2, 320);
  int draw_outline = array_int_at(info_arr, 3, 0) != 1;
  if (count < 0)
    count = 0;
  if (count > 256)
    count = 256;
  if (scale <= 0.0f)
    scale = 1.0f;

  int tex_w = next_pow2_clamped((int)ceilf((float)logical_w * scale));
  int tex_h = next_pow2_clamped((int)ceilf((float)logical_h * scale));
  static const int ox[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
  static const int oy[8] = {-1, -1, -1, 0, 0, 1, 1, 1};

  if (getenv("KOF_FONTLOG")) {
    debugPrintf("[font] onTextDraw count=%d tex=%dx%d scale=%.3f outline=%d\n",
                count, tex_w, tex_h, scale, draw_outline ? 1 : 0);
  }

  for (int i = 0; i < count; i++) {
    const char *text = string_array_at(text_arr, i);
    if (!text || !*text)
      continue;
    int src_size = array_int_at(size_arr, i, 16);
    int size = (int)ceilf((float)src_size * scale);
    if (size < 4)
      size = 4;
    if (size > 96)
      size = 96;
    int ascent = -font_ascent_px(size);
    float x = (float)array_int_at(x_arr, i, 0) * scale;
    float y = (float)array_int_at(y_arr, i, 0) * scale;
    float baseline = y + (float)ascent;

    if (draw_outline) {
      for (int n = 0; n < 8; n++) {
        draw_text_ttf(text, x + (float)ox[n] * scale,
                      baseline + (float)oy[n] * scale, size, 0xff000000,
                      tex_w, tex_h);
      }
    }

    draw_text_ttf(text, x, baseline, size,
                  array_int_at(color_arr, i, 0xffffffff), tex_w, tex_h);
  }

  if (getenv("KOF_FONTLOG"))
    debugPrintf("[font] onTextDraw done count=%d tex=%dx%d\n", count, tex_w,
                tex_h);
  return g_text_pixels_array;
}

static float jvalue_float(uint64_t v) {
  uint32_t bits = (uint32_t)v;
  float f;
  memcpy(&f, &bits, sizeof(f));
  return f;
}

static void *read_asset_bytes(const char *name, int off, int len) {
  char path[2048];
  FILE *f = NULL;
  const char *n = name ? name : "";

  if (snprintf(path, sizeof(path), "./assets/%s", n) < (int)sizeof(path))
    f = fopen(path, "rb");
  if (!f && snprintf(path, sizeof(path), "./assets/data/%s", n) < (int)sizeof(path))
    f = fopen(path, "rb");
  if (!f)
    f = fopen(n, "rb");
  if (!f) {
    debugPrintf("[jni] getFileDataJava missing: %s\n", n);
    return array_new(ARR_BYTE, 0, NULL);
  }

  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  if (off < 0)
    off = 0;
  if (off > size)
    off = (int)size;
  if (len < 0 || off + len > size)
    len = (int)size - off;
  unsigned char *buf = len > 0 ? malloc((size_t)len) : NULL;
  fseek(f, off, SEEK_SET);
  if (len > 0)
    fread(buf, 1, (size_t)len, f);
  fclose(f);
  void *arr = array_new(ARR_BYTE, len, buf);
  free(buf);
  return arr;
}

static int tag_for_name(const char *name) {
  if (!name) return MID_GENERIC;
  if (!strcmp(name, "getLanguage")) return MID_GET_LANGUAGE;
  if (!strcmp(name, "getAndroidID")) return MID_GET_ANDROID_ID;
  if (!strcmp(name, "getAppVersionName")) return MID_GET_APP_VERSION_NAME;
  if (!strcmp(name, "getMountedObbPath")) return MID_GET_MOUNTED_OBB_PATH;
  if (!strcmp(name, "isExpansionFileNative")) return MID_IS_EXPANSION_FILE_NATIVE;
  if (!strcmp(name, "mountExpansionFileNative")) return MID_MOUNT_EXPANSION_FILE_NATIVE;
  if (!strcmp(name, "unmountExpansionFileNative")) return MID_UNMOUNT_EXPANSION_FILE_NATIVE;
  if (!strcmp(name, "isMediaMounted")) return MID_IS_MEDIA_MOUNTED;
  if (!strcmp(name, "getKeyState")) return MID_GET_KEY_STATE;
  if (!strcmp(name, "getKeyTrigger")) return MID_GET_KEY_TRIGGER;
  if (!strcmp(name, "clearKeyTrigger")) return MID_CLEAR_KEY_TRIGGER;
  if (!strcmp(name, "getFileDataJava")) return MID_GET_FILE_DATA_JAVA;
  if (!strcmp(name, "getFontAscentJava")) return MID_GET_FONT_ASCENT;
  if (!strcmp(name, "getFontDescentJava")) return MID_GET_FONT_DESCENT;
  if (!strcmp(name, "getFontHeightJava")) return MID_GET_FONT_HEIGHT;
  if (!strcmp(name, "getFontWidthJava")) return MID_GET_FONT_WIDTH;
  if (!strcmp(name, "onTextDraw")) return MID_ON_TEXT_DRAW;
  if (!strcmp(name, "onTest")) return MID_ON_TEST;
  if (!strcmp(name, "setMovie")) return MID_SET_MOVIE;
  if (!strcmp(name, "playMovie")) return MID_PLAY_MOVIE;
  if (!strcmp(name, "stopMovie")) return MID_STOP_MOVIE;
  if (!strcmp(name, "isMovieFinished")) return MID_IS_MOVIE_FINISHED;
  if (!strcmp(name, "isMoviePlaying")) return MID_IS_MOVIE_PLAYING;
  if (!strcmp(name, "getMoviePosition")) return MID_GET_MOVIE_POSITION;
  if (!strcmp(name, "openURL")) return MID_OPEN_URL;
  if (!strcmp(name, "tweet")) return MID_TWEET;
  if (!strcmp(name, "loginTwitter")) return MID_LOGIN_TWITTER;
  if (!strcmp(name, "isTwitterTokenEnable")) return MID_IS_TWITTER_TOKEN_ENABLE;
  if (!strcmp(name, "isTwitterConnecting")) return MID_IS_TWITTER_CONNECTING;
  if (!strcmp(name, "createIndicator")) return MID_CREATE_INDICATOR;
  if (!strcmp(name, "deleteIndicator")) return MID_DELETE_INDICATOR;
  if (!strcmp(name, "isActiveIndicator")) return MID_IS_ACTIVE_INDICATOR;
  if (!strcmp(name, "startIndicator")) return MID_START_INDICATOR;
  if (!strcmp(name, "stopIndicator")) return MID_STOP_INDICATOR;
  if (!strcmp(name, "createOKDialog")) return MID_CREATE_OK_DIALOG;
  if (!strcmp(name, "isDialogVisible")) return MID_IS_DIALOG_VISIBLE;
  if (!strcmp(name, "getDialogReturnVal")) return MID_GET_DIALOG_RETURN_VAL;
  return MID_GENERIC;
}

static jint jni_GetVersion(void *env) { (void)env; return 0x00010006; }

static void *jni_FindClass(void *env, const char *name) {
  (void)env;
  if (getenv("KOF_JNILOG"))
    debugPrintf("[jni] FindClass(%s)\n", name ? name : "(null)");
  static int fake_class;
  return &fake_class;
}

static jint jni_GetJavaVM(void *env, void **vm) {
  (void)env;
  if (vm) *vm = &java_vm_ptr;
  return 0;
}

static void *jni_GetStaticMethodID(void *env, void *clazz, const char *name,
                                   const char *sig) {
  (void)env; (void)clazz;
  int tag = tag_for_name(name);
  if (getenv("KOF_JNILOG") || tag == MID_GENERIC)
    debugPrintf("[jni] GetStaticMethodID(%s, %s) -> %d\n",
                name ? name : "(null)", sig ? sig : "(null)", tag);
  return &g_method_tags[tag];
}

static void *jni_GetMethodID(void *env, void *clazz, const char *name,
                             const char *sig) {
  (void)env; (void)clazz;
  debugPrintf("[jni] GetMethodID(%s, %s)\n", name ? name : "(null)",
              sig ? sig : "(null)");
  return &g_method_tags[MID_GENERIC];
}

static void *jni_CallStaticObjectMethodV(void *env, void *clazz, void *mid,
                                         va_list ap) {
  (void)env; (void)clazz;
  if (mid == &g_method_tags[MID_GET_LANGUAGE])
    return jni_make_string("en");
  if (mid == &g_method_tags[MID_GET_ANDROID_ID])
    return jni_make_string("nextos-kof2012a");
  if (mid == &g_method_tags[MID_GET_APP_VERSION_NAME])
    return jni_make_string("1.0.8");
  if (mid == &g_method_tags[MID_GET_MOUNTED_OBB_PATH])
    return jni_make_string("./assets");
  if (mid == &g_method_tags[MID_GET_FILE_DATA_JAVA]) {
    void *jname = va_arg(ap, void *);
    int off = va_arg(ap, int);
    int len = va_arg(ap, int);
    return read_asset_bytes(string_value(jname), off, len);
  }
  if (mid == &g_method_tags[MID_ON_TEXT_DRAW]) {
    void *info = va_arg(ap, void *);
    float scale = (float)va_arg(ap, double);
    void *sizes = va_arg(ap, void *);
    void *colors = va_arg(ap, void *);
    void *xs = va_arg(ap, void *);
    void *ys = va_arg(ap, void *);
    void *texts = va_arg(ap, void *);
    return draw_text_bitmap(info, scale, sizes, colors, xs, ys, texts);
  }
  if (mid == &g_method_tags[MID_ON_TEST]) {
    void *arr = va_arg(ap, void *);
    return arr ? arr : kof_jni_int_array(NULL, 0);
  }
  debugPrintf("[jni] CallStaticObjectMethodV unknown mid=%p\n", mid);
  return NULL;
}

static void *jni_CallStaticObjectMethod(void *env, void *clazz, void *mid, ...) {
  va_list ap; va_start(ap, mid);
  void *ret = jni_CallStaticObjectMethodV(env, clazz, mid, ap);
  va_end(ap);
  return ret;
}

static void *jni_CallStaticObjectMethodA(void *env, void *clazz, void *mid,
                                         const uint64_t *args) {
  (void)env; (void)clazz;
  if (mid == &g_method_tags[MID_GET_LANGUAGE]) return jni_make_string("en");
  if (mid == &g_method_tags[MID_GET_ANDROID_ID]) return jni_make_string("nextos-kof2012a");
  if (mid == &g_method_tags[MID_GET_APP_VERSION_NAME]) return jni_make_string("1.0.8");
  if (mid == &g_method_tags[MID_GET_MOUNTED_OBB_PATH]) return jni_make_string("./assets");
  if (mid == &g_method_tags[MID_GET_FILE_DATA_JAVA])
    return args ? read_asset_bytes(string_value((void *)args[0]), (int)args[1], (int)args[2])
                : array_new(ARR_BYTE, 0, NULL);
  if (mid == &g_method_tags[MID_ON_TEXT_DRAW]) {
    return args ? draw_text_bitmap((void *)args[0], jvalue_float(args[1]),
                                   (void *)args[2], (void *)args[3],
                                   (void *)args[4], (void *)args[5],
                                   (void *)args[6])
                : draw_text_bitmap(NULL, 1.0f, NULL, NULL, NULL, NULL, NULL);
  }
  return NULL;
}

static jint jni_CallStaticIntMethodV(void *env, void *clazz, void *mid,
                                     va_list ap) {
  (void)env; (void)clazz;
  if (mid == &g_method_tags[MID_GET_KEY_STATE]) {
    static unsigned n = 0;
    if (getenv("KOF_INPUT_LOG") && (n++ % 120 == 0))
      debugPrintf("JNI getKeyState #%u = 0x%x\n", n, g_key_state);
    return g_key_state;
  }
  if (mid == &g_method_tags[MID_GET_KEY_TRIGGER]) {
    static unsigned n = 0;
    if (getenv("KOF_INPUT_LOG") && (n++ % 120 == 0))
      debugPrintf("JNI getKeyTrigger #%u = 0x%x\n", n, g_key_trigger);
    return g_key_trigger;
  }
  if (mid == &g_method_tags[MID_IS_EXPANSION_FILE_NATIVE]) return 1;
  if (mid == &g_method_tags[MID_GET_DIALOG_RETURN_VAL]) return 0;
  if (mid == &g_method_tags[MID_GET_MOVIE_POSITION]) return kof_jni_movie_position_ms();
  if (mid == &g_method_tags[MID_GET_FONT_ASCENT]) {
    int size = va_arg(ap, int);
    return font_ascent_px(size);
  }
  if (mid == &g_method_tags[MID_GET_FONT_DESCENT]) {
    int size = va_arg(ap, int);
    return font_descent_px(size);
  }
  if (mid == &g_method_tags[MID_GET_FONT_HEIGHT]) {
    int size = va_arg(ap, int);
    return -font_ascent_px(size) + font_descent_px(size);
  }
  if (mid == &g_method_tags[MID_GET_FONT_WIDTH]) {
    int size = va_arg(ap, int);
    void *jstr = va_arg(ap, void *);
    return font_width_px(size, string_value(jstr));
  }
  return 0;
}

static jint jni_CallStaticIntMethod(void *env, void *clazz, void *mid, ...) {
  va_list ap; va_start(ap, mid);
  jint ret = jni_CallStaticIntMethodV(env, clazz, mid, ap);
  va_end(ap);
  return ret;
}

static jint jni_CallStaticIntMethodA(void *env, void *clazz, void *mid,
                                     const uint64_t *args) {
  (void)env; (void)clazz;
  if (mid == &g_method_tags[MID_GET_KEY_STATE]) {
    static unsigned n = 0;
    if (getenv("KOF_INPUT_LOG") && (n++ % 120 == 0))
      debugPrintf("JNI getKeyState #%u = 0x%x\n", n, g_key_state);
    return g_key_state;
  }
  if (mid == &g_method_tags[MID_GET_KEY_TRIGGER]) {
    static unsigned n = 0;
    if (getenv("KOF_INPUT_LOG") && (n++ % 120 == 0))
      debugPrintf("JNI getKeyTrigger #%u = 0x%x\n", n, g_key_trigger);
    return g_key_trigger;
  }
  if (mid == &g_method_tags[MID_IS_EXPANSION_FILE_NATIVE]) return 1;
  if (mid == &g_method_tags[MID_GET_DIALOG_RETURN_VAL]) return 0;
  if (mid == &g_method_tags[MID_GET_MOVIE_POSITION]) return kof_jni_movie_position_ms();
  if (mid == &g_method_tags[MID_GET_FONT_ASCENT])
    return font_ascent_px(args ? (int)args[0] : 16);
  if (mid == &g_method_tags[MID_GET_FONT_DESCENT])
    return font_descent_px(args ? (int)args[0] : 16);
  if (mid == &g_method_tags[MID_GET_FONT_HEIGHT]) {
    int size = args ? (int)args[0] : 16;
    return -font_ascent_px(size) + font_descent_px(size);
  }
  if (mid == &g_method_tags[MID_GET_FONT_WIDTH]) {
    int size = args ? (int)args[0] : 16;
    void *jstr = args ? (void *)args[1] : NULL;
    return font_width_px(size, string_value(jstr));
  }
  return 0;
}

static jboolean jni_CallStaticBooleanMethodV(void *env, void *clazz, void *mid,
                                             va_list ap) {
  (void)env; (void)clazz; (void)ap;
  if (mid == &g_method_tags[MID_MOUNT_EXPANSION_FILE_NATIVE]) return 1;
  if (mid == &g_method_tags[MID_UNMOUNT_EXPANSION_FILE_NATIVE]) return 1;
  if (mid == &g_method_tags[MID_IS_MEDIA_MOUNTED]) return 1;
  if (mid == &g_method_tags[MID_IS_MOVIE_FINISHED]) {
    movie_update_finished();
    return g_movie_state == 5;
  }
  if (mid == &g_method_tags[MID_IS_MOVIE_PLAYING])
    return kof_jni_movie_is_playing();
  if (mid == &g_method_tags[MID_IS_TWITTER_TOKEN_ENABLE]) return 0;
  if (mid == &g_method_tags[MID_IS_TWITTER_CONNECTING]) return 0;
  if (mid == &g_method_tags[MID_IS_ACTIVE_INDICATOR]) return 0;
  if (mid == &g_method_tags[MID_IS_DIALOG_VISIBLE]) return 0;
  return 0;
}

static jboolean jni_CallStaticBooleanMethod(void *env, void *clazz, void *mid, ...) {
  va_list ap; va_start(ap, mid);
  jboolean ret = jni_CallStaticBooleanMethodV(env, clazz, mid, ap);
  va_end(ap);
  return ret;
}

static jboolean jni_CallStaticBooleanMethodA(void *env, void *clazz, void *mid,
                                             const uint64_t *args) {
  (void)args;
  va_list ap;
  memset(&ap, 0, sizeof(ap));
  return jni_CallStaticBooleanMethodV(env, clazz, mid, ap);
}

static void jni_CallStaticVoidMethodV(void *env, void *clazz, void *mid,
                                      va_list ap) {
  (void)env; (void)clazz;
  if (mid == &g_method_tags[MID_CLEAR_KEY_TRIGGER]) {
    kof_jni_clear_key_trigger();
    return;
  }
  if (mid == &g_method_tags[MID_SET_MOVIE]) {
    void *jname = va_arg(ap, void *);
    snprintf(g_movie_name, sizeof(g_movie_name), "%s", string_value(jname));
    g_movie_duration_ms = movie_duration_for_name(g_movie_name);
    g_movie_start_ms = 0;
    g_movie_state = 1;
    if (getenv("KOF_JNILOG"))
      debugPrintf("[jni] setMovie(%s) duration=%d\n",
                  g_movie_name, g_movie_duration_ms);
    return;
  }
  if (mid == &g_method_tags[MID_PLAY_MOVIE]) {
    g_movie_start_ms = monotonic_ms();
    g_movie_state = 2;
    if (getenv("KOF_JNILOG"))
      debugPrintf("[jni] playMovie(%s)\n", g_movie_name);
    return;
  }
  if (mid == &g_method_tags[MID_STOP_MOVIE]) {
    g_movie_state = 5;
    if (getenv("KOF_JNILOG"))
      debugPrintf("[jni] stopMovie(%s)\n", g_movie_name);
    return;
  }
  if (mid == &g_method_tags[MID_OPEN_URL] || mid == &g_method_tags[MID_TWEET]) {
    void *jstr = va_arg(ap, void *);
    debugPrintf("[jni] ignored java action: %s\n", string_value(jstr));
    return;
  }
  if (mid == &g_method_tags[MID_LOGIN_TWITTER] ||
      mid == &g_method_tags[MID_CREATE_INDICATOR] ||
      mid == &g_method_tags[MID_DELETE_INDICATOR] ||
      mid == &g_method_tags[MID_START_INDICATOR] ||
      mid == &g_method_tags[MID_STOP_INDICATOR] ||
      mid == &g_method_tags[MID_CREATE_OK_DIALOG])
    return;
}

static void jni_CallStaticVoidMethod(void *env, void *clazz, void *mid, ...) {
  va_list ap; va_start(ap, mid);
  jni_CallStaticVoidMethodV(env, clazz, mid, ap);
  va_end(ap);
}

static void jni_CallStaticVoidMethodA(void *env, void *clazz, void *mid,
                                      const uint64_t *args) {
  (void)args;
  va_list ap;
  memset(&ap, 0, sizeof(ap));
  jni_CallStaticVoidMethodV(env, clazz, mid, ap);
}

static void *jni_NewGlobalRef(void *env, void *obj) { (void)env; return obj; }
static void *jni_NewLocalRef(void *env, void *obj) { (void)env; return obj; }
static void jni_DeleteGlobalRef(void *env, void *obj) { (void)env; (void)obj; }
static void jni_DeleteLocalRef(void *env, void *obj) { (void)env; (void)obj; }
static void *jni_GetObjectClass(void *env, void *obj) { (void)env; (void)obj; static int c; return &c; }
static jboolean jni_IsInstanceOf(void *env, void *obj, void *clazz) { (void)env; (void)obj; (void)clazz; return 1; }

static void *jni_NewStringUTF(void *env, const char *str) {
  (void)env;
  return jni_make_string(str ? str : "");
}

static jint jni_GetStringUTFLength(void *env, void *jstr) {
  (void)env;
  return (jint)strlen(string_value(jstr));
}

static const char *jni_GetStringUTFChars(void *env, void *jstr, jboolean *isCopy) {
  (void)env;
  if (isCopy) *isCopy = 0;
  return string_value(jstr);
}

static void jni_ReleaseStringUTFChars(void *env, void *jstr, const char *chars) {
  (void)env; (void)jstr; (void)chars;
}

static jint jni_GetStringLength(void *env, void *jstr) {
  (void)env;
  return (jint)strlen(string_value(jstr));
}

static unsigned short *jni_GetStringChars(void *env, void *jstr, jboolean *isCopy) {
  (void)env;
  const char *s = string_value(jstr);
  size_t n = strlen(s);
  unsigned short *out = calloc(n + 1, sizeof(unsigned short));
  for (size_t i = 0; i < n; i++)
    out[i] = (unsigned char)s[i];
  if (isCopy) *isCopy = 1;
  return out;
}

static void jni_ReleaseStringChars(void *env, void *jstr, unsigned short *chars) {
  (void)env; (void)jstr;
  free(chars);
}

static jint jni_GetArrayLength(void *env, void *array) {
  (void)env;
  FakeArray *a = array_find(array);
  return a ? a->len : 0;
}

static void *jni_NewByteArray(void *env, jint len) { (void)env; return array_new(ARR_BYTE, len, NULL); }
static void *jni_NewIntArray(void *env, jint len) { (void)env; return array_new(ARR_INT, len, NULL); }
static void *jni_NewFloatArray(void *env, jint len) { (void)env; return array_new(ARR_FLOAT, len, NULL); }
static void *jni_NewObjectArray(void *env, jint len, void *clazz, void *initial) {
  (void)env; (void)clazz;
  void *arr = array_new(ARR_OBJECT, len, NULL);
  if (initial && len > 0) {
    FakeArray *a = array_find(arr);
    if (a && a->type == ARR_OBJECT && a->data) {
      void **items = (void **)a->data;
      for (int i = 0; i < len; i++)
        items[i] = initial;
    }
  }
  return arr;
}

static void *jni_GetArrayElements(void *env, void *array, jboolean *isCopy) {
  (void)env;
  FakeArray *a = array_find(array);
  if (isCopy) *isCopy = 0;
  return a ? a->data : NULL;
}

static void jni_ReleaseArrayElements(void *env, void *array, void *elems, jint mode) {
  (void)env; (void)array; (void)elems; (void)mode;
}

static void jni_GetByteArrayRegion(void *env, void *array, jint start, jint len, void *buf) {
  (void)env;
  FakeArray *a = array_find(array);
  if (!a || a->type != ARR_BYTE || !buf || start < 0 || len < 0 || start + len > a->len) return;
  memcpy(buf, (unsigned char *)a->data + start, len);
}

static void jni_SetByteArrayRegion(void *env, void *array, jint start, jint len, const void *buf) {
  (void)env;
  FakeArray *a = array_find(array);
  if (!a || a->type != ARR_BYTE || !buf || start < 0 || len < 0 || start + len > a->len) return;
  memcpy((unsigned char *)a->data + start, buf, len);
}

static void jni_GetIntArrayRegion(void *env, void *array, jint start, jint len, void *buf) {
  (void)env;
  FakeArray *a = array_find(array);
  if (!a || a->type != ARR_INT || !buf || start < 0 || len < 0 || start + len > a->len) return;
  memcpy(buf, (int *)a->data + start, (size_t)len * sizeof(int));
}

static void jni_SetIntArrayRegion(void *env, void *array, jint start, jint len, const void *buf) {
  (void)env;
  FakeArray *a = array_find(array);
  if (!a || a->type != ARR_INT || !buf || start < 0 || len < 0 || start + len > a->len) return;
  memcpy((int *)a->data + start, buf, (size_t)len * sizeof(int));
}

static void *jni_GetObjectArrayElement(void *env, void *array, jint idx) {
  (void)env;
  FakeArray *a = array_find(array);
  if (!a || a->type != ARR_OBJECT || idx < 0 || idx >= a->len) return jni_make_string("");
  return ((void **)a->data)[idx];
}

static void jni_SetObjectArrayElement(void *env, void *array, jint idx, void *value) {
  (void)env;
  FakeArray *a = array_find(array);
  if (!a || a->type != ARR_OBJECT || idx < 0 || idx >= a->len || !a->data)
    return;
  ((void **)a->data)[idx] = value;
}

static jboolean jni_ExceptionCheck(void *env) { (void)env; return 0; }
static void jni_ExceptionClear(void *env) { (void)env; }
static void *jni_ExceptionOccurred(void *env) { (void)env; return NULL; }
static void jni_ExceptionDescribe(void *env) { (void)env; }
static jint jni_Throw(void *env, void *obj) { (void)env; (void)obj; return 0; }
static jint jni_ThrowNew(void *env, void *clazz, const char *msg) { (void)env; (void)clazz; (void)msg; return 0; }

static jint vm_DestroyJavaVM(void *vm) { (void)vm; return 0; }
static jint vm_AttachCurrentThread(void *vm, void **penv, void *args) {
  (void)vm; (void)args;
  if (penv) *penv = &jni_env_ptr;
  return 0;
}
static jint vm_DetachCurrentThread(void *vm) { (void)vm; return 0; }
static jint vm_GetEnv(void *vm, void **penv, jint version) {
  (void)vm; (void)version;
  if (penv) *penv = &jni_env_ptr;
  return 0;
}

void jni_shim_init(void **out_vm, void **out_env) {
  for (int i = 0; i < JNI_VTABLE_SIZE; i++) {
    jni_env_vtable[i] = (uintptr_t)jni_stub;
    java_vm_vtable[i] = (uintptr_t)jni_stub;
  }

  jni_env_vtable[4] = (uintptr_t)jni_GetVersion;
  jni_env_vtable[6] = (uintptr_t)jni_FindClass;
  jni_env_vtable[13] = (uintptr_t)jni_Throw;
  jni_env_vtable[14] = (uintptr_t)jni_ThrowNew;
  jni_env_vtable[15] = (uintptr_t)jni_ExceptionOccurred;
  jni_env_vtable[16] = (uintptr_t)jni_ExceptionDescribe;
  jni_env_vtable[17] = (uintptr_t)jni_ExceptionClear;
  jni_env_vtable[21] = (uintptr_t)jni_NewGlobalRef;
  jni_env_vtable[22] = (uintptr_t)jni_DeleteGlobalRef;
  jni_env_vtable[23] = (uintptr_t)jni_DeleteLocalRef;
  jni_env_vtable[25] = (uintptr_t)jni_NewLocalRef;
  jni_env_vtable[31] = (uintptr_t)jni_GetObjectClass;
  jni_env_vtable[32] = (uintptr_t)jni_IsInstanceOf;
  jni_env_vtable[33] = (uintptr_t)jni_GetMethodID;
  jni_env_vtable[111] = (uintptr_t)jni_GetStaticMethodID;
  jni_env_vtable[113] = (uintptr_t)jni_GetStaticMethodID;

  jni_env_vtable[114] = (uintptr_t)jni_CallStaticObjectMethod;
  jni_env_vtable[115] = (uintptr_t)jni_CallStaticObjectMethodV;
  jni_env_vtable[116] = (uintptr_t)jni_CallStaticObjectMethodA;
  jni_env_vtable[117] = (uintptr_t)jni_CallStaticBooleanMethod;
  jni_env_vtable[118] = (uintptr_t)jni_CallStaticBooleanMethodV;
  jni_env_vtable[119] = (uintptr_t)jni_CallStaticBooleanMethodA;
  jni_env_vtable[129] = (uintptr_t)jni_CallStaticIntMethod;
  jni_env_vtable[130] = (uintptr_t)jni_CallStaticIntMethodV;
  jni_env_vtable[131] = (uintptr_t)jni_CallStaticIntMethodA;
  jni_env_vtable[141] = (uintptr_t)jni_CallStaticVoidMethod;
  jni_env_vtable[142] = (uintptr_t)jni_CallStaticVoidMethodV;
  jni_env_vtable[143] = (uintptr_t)jni_CallStaticVoidMethodA;

  jni_env_vtable[164] = (uintptr_t)jni_GetStringLength;
  jni_env_vtable[165] = (uintptr_t)jni_GetStringChars;
  jni_env_vtable[166] = (uintptr_t)jni_ReleaseStringChars;
  jni_env_vtable[167] = (uintptr_t)jni_NewStringUTF;
  jni_env_vtable[168] = (uintptr_t)jni_GetStringUTFLength;
  jni_env_vtable[169] = (uintptr_t)jni_GetStringUTFChars;
  jni_env_vtable[170] = (uintptr_t)jni_ReleaseStringUTFChars;
  jni_env_vtable[171] = (uintptr_t)jni_GetArrayLength;
  jni_env_vtable[172] = (uintptr_t)jni_NewObjectArray;
  jni_env_vtable[173] = (uintptr_t)jni_GetObjectArrayElement;
  jni_env_vtable[174] = (uintptr_t)jni_SetObjectArrayElement;
  jni_env_vtable[176] = (uintptr_t)jni_NewByteArray;
  jni_env_vtable[179] = (uintptr_t)jni_NewIntArray;
  jni_env_vtable[181] = (uintptr_t)jni_NewFloatArray;
  jni_env_vtable[184] = (uintptr_t)jni_GetArrayElements;
  jni_env_vtable[187] = (uintptr_t)jni_GetArrayElements;
  jni_env_vtable[189] = (uintptr_t)jni_GetArrayElements;
  jni_env_vtable[192] = (uintptr_t)jni_ReleaseArrayElements;
  jni_env_vtable[195] = (uintptr_t)jni_ReleaseArrayElements;
  jni_env_vtable[197] = (uintptr_t)jni_ReleaseArrayElements;
  jni_env_vtable[200] = (uintptr_t)jni_GetByteArrayRegion;
  jni_env_vtable[203] = (uintptr_t)jni_GetIntArrayRegion;
  jni_env_vtable[205] = (uintptr_t)jni_ExceptionCheck;
  jni_env_vtable[208] = (uintptr_t)jni_SetByteArrayRegion;
  jni_env_vtable[211] = (uintptr_t)jni_SetIntArrayRegion;
  jni_env_vtable[219] = (uintptr_t)jni_GetJavaVM;

  java_vm_vtable[3] = (uintptr_t)vm_DestroyJavaVM;
  java_vm_vtable[4] = (uintptr_t)vm_AttachCurrentThread;
  java_vm_vtable[5] = (uintptr_t)vm_DetachCurrentThread;
  java_vm_vtable[6] = (uintptr_t)vm_GetEnv;

  jni_env_ptr = jni_env_vtable;
  java_vm_ptr = java_vm_vtable;
  if (out_vm) *out_vm = &java_vm_ptr;
  if (out_env) *out_env = &jni_env_ptr;
}
