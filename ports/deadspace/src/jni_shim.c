#define _GNU_SOURCE
#include "jni_shim.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define JNI_VTABLE_SIZE 512

typedef int jint;
typedef long long jlong;
typedef unsigned char jboolean;

static uintptr_t jni_env_vtable[JNI_VTABLE_SIZE];
static void *jni_env_ptr;
static uintptr_t java_vm_vtable[JNI_VTABLE_SIZE];
static void *java_vm_ptr;

static char g_asset_root[512] = "assets";
static int g_screen_w = 1280;
static int g_screen_h = 720;
static int g_jni_log = 0;
static int g_asset_log = 0;

static void (*g_audio_write_cb)(const int16_t *samples, int sample_count);
static void (*g_audio_state_cb)(int playing);
static unsigned g_audio_write_calls;

enum {
  MAGIC_STRING = 0x44533153,
  MAGIC_ARRAY  = 0x44533141,
  MAGIC_STREAM = 0x44533149,
  MAGIC_AFD    = 0x44533146,
};

typedef struct {
  uint32_t magic;
  char *s;
} JString;

typedef struct {
  uint32_t magic;
  int kind;       /* 1=byte, 2=short, 3=object */
  int len;        /* elements, not bytes */
  unsigned char *data;
  void **objects;
} JArray;

typedef struct {
  uint32_t magic;
  FILE *fp;
  long len;
  char path[1024];
} JStream;

typedef struct {
  uint32_t magic;
  long len;
  char path[1024];
} JAFD;

static int g_class_activity, g_class_renderer, g_class_asset_manager, g_class_audio;
static int g_activity_obj, g_renderer_obj, g_asset_manager_obj, g_audio_obj;

void *jni_activity_object(void) { return &g_activity_obj; }
void *jni_renderer_object(void) { return &g_renderer_obj; }
void *jni_asset_manager(void) { return &g_asset_manager_obj; }
void *jni_audio_track_object(void) { return &g_audio_obj; }

void jni_set_asset_root(const char *path) {
  if (path && *path) {
    strncpy(g_asset_root, path, sizeof(g_asset_root) - 1);
    g_asset_root[sizeof(g_asset_root) - 1] = 0;
  }
}
void jni_set_display_size(int w, int h) {
  if (w > 0) g_screen_w = w;
  if (h > 0) g_screen_h = h;
}
void jni_set_audio_output(void (*write_cb)(const int16_t *, int), void (*state_cb)(int)) {
  g_audio_write_cb = write_cb;
  g_audio_state_cb = state_cb;
}

void *jni_make_string(const char *value) {
  JString *s = calloc(1, sizeof(*s));
  s->magic = MAGIC_STRING;
  s->s = strdup(value ? value : "");
  return s;
}

static const char *jstr(void *h) {
  JString *s = (JString *)h;
  if (!s || s->magic != MAGIC_STRING) return "";
  return s->s ? s->s : "";
}

static JArray *array_new(int kind, int len) {
  JArray *a = calloc(1, sizeof(*a));
  a->magic = MAGIC_ARRAY;
  a->kind = kind;
  a->len = len < 0 ? 0 : len;
  if (kind == 1) a->data = calloc(a->len ? a->len : 1, 1);
  else if (kind == 2) a->data = calloc(a->len ? a->len : 1, 2);
  else if (kind == 3) a->objects = calloc(a->len ? a->len : 1, sizeof(void *));
  return a;
}

unsigned char *jni_shim_get_array(void *handle, int *out_len) {
  JArray *a = (JArray *)handle;
  if (!a || a->magic != MAGIC_ARRAY || !a->data) {
    if (out_len) *out_len = 0;
    return NULL;
  }
  if (out_len) *out_len = a->len * (a->kind == 2 ? 2 : 1);
  return a->data;
}

static int path_join(char *out, size_t outsz, const char *name, int variant) {
  const char *n = name ? name : "";
  while (*n == '/') n++;
  if (strncmp(n, "assets/", 7) == 0) n += 7;
  if (variant == 0)
    return snprintf(out, outsz, "%s/%s", g_asset_root, n);
  if (variant == 1 && strncmp(n, "published/", 10) != 0)
    return snprintf(out, outsz, "%s/published/%s", g_asset_root, n);
  return snprintf(out, outsz, "%s/%s", g_asset_root, n);
}

static FILE *open_asset_file(const char *name, char *resolved, size_t rsz, long *len) {
  char path[1024];
  for (int i = 0; i < 2; i++) {
    path_join(path, sizeof(path), name, i);
    FILE *fp = fopen(path, "rb");
    if (!fp) continue;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (resolved) snprintf(resolved, rsz, "%s", path);
    if (len) *len = n;
    if (g_asset_log) fprintf(stderr, "[asset] open %s (%ld)\n", path, n);
    return fp;
  }
  if (g_asset_log) fprintf(stderr, "[asset] MISS %s\n", name ? name : "");
  return NULL;
}

static void *stream_open(const char *name) {
  JStream *st = calloc(1, sizeof(*st));
  st->magic = MAGIC_STREAM;
  st->fp = open_asset_file(name, st->path, sizeof(st->path), &st->len);
  if (!st->fp) {
    free(st);
    return NULL;
  }
  return st;
}

static void stream_close(void *obj) {
  JStream *st = (JStream *)obj;
  if (st && st->magic == MAGIC_STREAM) {
    if (st->fp) fclose(st->fp);
    st->fp = NULL;
  }
}

static void *afd_open(const char *name) {
  long len = 0;
  char path[1024];
  FILE *fp = open_asset_file(name, path, sizeof(path), &len);
  if (fp) fclose(fp);
  if (!fp) return NULL;
  JAFD *afd = calloc(1, sizeof(*afd));
  afd->magic = MAGIC_AFD;
  afd->len = len;
  snprintf(afd->path, sizeof(afd->path), "%s", path);
  return afd;
}

static void *asset_list(const char *name) {
  char path[1024];
  DIR *d = NULL;
  for (int i = 0; i < 2 && !d; i++) {
    path_join(path, sizeof(path), name, i);
    d = opendir(path);
  }
  if (!d) {
    if (g_asset_log) fprintf(stderr, "[asset] list MISS %s\n", name ? name : "");
    return array_new(3, 0);
  }
  int cap = 32, n = 0;
  char **names = calloc(cap, sizeof(char *));
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.') continue;
    if (n >= cap) {
      cap *= 2;
      names = realloc(names, cap * sizeof(char *));
    }
    names[n++] = strdup(e->d_name);
  }
  closedir(d);
  JArray *arr = array_new(3, n);
  for (int i = 0; i < n; i++) {
    arr->objects[i] = jni_make_string(names[i]);
    free(names[i]);
  }
  free(names);
  if (g_asset_log) fprintf(stderr, "[asset] list %s -> %d\n", path, n);
  return arr;
}

enum {
  MID_GENERIC = 0,
  MID_ASSET_OPEN,
  MID_ASSET_LIST,
  MID_ASSET_OPENFD,
  MID_INPUT_READ1,
  MID_INPUT_READ3,
  MID_INPUT_SKIP,
  MID_INPUT_CLOSE,
  MID_INPUT_AVAILABLE,
  MID_AFD_GET_LENGTH,
  MID_AUDIO_WRITE,
  MID_AUDIO_PLAY,
  MID_AUDIO_STOP,
  MID_AUDIO_RELEASE,
  MID_AUDIO_FLUSH,
  MID_GET_ASSETS,
  MID_GET_INSTANCE,
  MID_STR_GENERIC,
  MID_STR_ZERO,
  MID_STR_ONE,
  MID_STR_APPDIR,
  MID_STR_EXTDIR,
  MID_STR_LANGUAGE,
  MID_STR_LOCALE,
  MID_STR_API,
  MID_STR_RAM,
  MID_STR_VERSION,
  MID_INT_WIDTH,
  MID_INT_HEIGHT,
  MID_INT_ORIENTATION,
  MID_FLOAT_DPI,
  MID_BOOL_TRUE,
  MID_LONG_MEMORY,
  MID_EXIT,
  MID_COUNT
};
static int g_method_tags[MID_COUNT];

static intptr_t jni_stub(void) { return 0; }
static jint jni_GetVersion(void *env) { (void)env; return 0x00010006; }

static void *jni_FindClass(void *env, const char *name) {
  (void)env;
  if (g_jni_log) fprintf(stderr, "[jni] FindClass(%s)\n", name ? name : "");
  if (name && strstr(name, "AssetManager")) return &g_class_asset_manager;
  if (name && strstr(name, "AudioTrack")) return &g_class_audio;
  if (name && strstr(name, "AndroidRenderer")) return &g_class_renderer;
  return &g_class_activity;
}

static int starts_with(const char *s, const char *p) {
  return s && p && strncmp(s, p, strlen(p)) == 0;
}

static int tag_for_method(const char *name, const char *sig) {
  if (!name) return MID_GENERIC;
  if (strcmp(name, "open") == 0) return MID_ASSET_OPEN;
  if (strcmp(name, "list") == 0) return MID_ASSET_LIST;
  if (strcmp(name, "openFd") == 0) return MID_ASSET_OPENFD;
  if (strcmp(name, "read") == 0) {
    if (sig && strstr(sig, "II")) return MID_INPUT_READ3;
    return MID_INPUT_READ1;
  }
  if (strcmp(name, "skip") == 0) return MID_INPUT_SKIP;
  if (strcmp(name, "close") == 0) return MID_INPUT_CLOSE;
  if (strcmp(name, "available") == 0) return MID_INPUT_AVAILABLE;
  if (strcmp(name, "getLength") == 0) return MID_AFD_GET_LENGTH;
  if (strcmp(name, "write") == 0) return MID_AUDIO_WRITE;
  if (strcmp(name, "play") == 0) return MID_AUDIO_PLAY;
  if (strcmp(name, "stop") == 0 || strcmp(name, "pause") == 0) return MID_AUDIO_STOP;
  if (strcmp(name, "release") == 0) return MID_AUDIO_RELEASE;
  if (strcmp(name, "flush") == 0) return MID_AUDIO_FLUSH;
  if (strcmp(name, "getAssets") == 0) return MID_GET_ASSETS;
  if (strcmp(name, "GetInstance") == 0) return MID_GET_INSTANCE;
  if (strcmp(name, "exit") == 0 || strstr(name, "finish") || strstr(name, "terminate")) return MID_EXIT;
  if (strcmp(name, "GetDefaultWidth") == 0 || strcmp(name, "getWidth") == 0) return MID_INT_WIDTH;
  if (strcmp(name, "GetDefaultHeight") == 0 || strcmp(name, "getHeight") == 0) return MID_INT_HEIGHT;
  if (strstr(name, "Orientation")) return MID_INT_ORIENTATION;
  if (strstr(name, "Dpi") || strstr(name, "DPI")) return MID_FLOAT_DPI;
  if (strcmp(name, "GetAppDataDirectory") == 0) return MID_STR_APPDIR;
  if (strcmp(name, "GetExternalStorageDirectory") == 0) return MID_STR_EXTDIR;
  if (strcmp(name, "GetLanguage") == 0) return MID_STR_LANGUAGE;
  if (strcmp(name, "GetLocale") == 0) return MID_STR_LOCALE;
  if (strcmp(name, "GetApiLevel") == 0) return MID_STR_API;
  if (strcmp(name, "GetTotalRAM") == 0) return MID_STR_RAM;
  if (strcmp(name, "getVersion") == 0) return MID_STR_VERSION;
  if (strcmp(name, "getTotalMemory") == 0) return MID_LONG_MEMORY;
  if (strstr(name, "DisplayCount") || strstr(name, "TouchScreenCount") ||
      strstr(name, "TouchPadCount") || strstr(name, "PhysicalKeyboardCount"))
    return MID_STR_ONE;
  if (strstr(name, "Count") || strstr(name, "Available")) return MID_STR_ZERO;
  if (starts_with(name, "is") || starts_with(name, "Is") || starts_with(name, "has") ||
      starts_with(name, "Has") || strstr(name, "Ready")) return MID_BOOL_TRUE;
  if (strstr(name, "get") || strstr(name, "Get") || strstr(name, "toString")) return MID_STR_GENERIC;
  return MID_GENERIC;
}

static void *jni_GetMethodID(void *env, void *clazz, const char *name, const char *sig) {
  (void)env;
  (void)clazz;
  int tag = tag_for_method(name, sig);
  if (g_jni_log) fprintf(stderr, "[jni] GetMethodID(%s,%s) -> %d\n", name ? name : "", sig ? sig : "", tag);
  return &g_method_tags[tag];
}
static void *jni_GetStaticMethodID(void *env, void *clazz, const char *name, const char *sig) {
  return jni_GetMethodID(env, clazz, name, sig);
}

static void *string_for_generic(void) {
  return jni_make_string("nextos");
}
static void *string_for_tag(void *mid) {
  if (mid == &g_method_tags[MID_STR_ZERO]) return jni_make_string("0");
  if (mid == &g_method_tags[MID_STR_ONE]) return jni_make_string("1");
  if (mid == &g_method_tags[MID_STR_APPDIR] || mid == &g_method_tags[MID_STR_EXTDIR]) {
    const char *home = getenv("DEADSPACE_HOME");
    return jni_make_string((home && *home) ? home : ".");
  }
  if (mid == &g_method_tags[MID_STR_LANGUAGE]) return jni_make_string("en");
  if (mid == &g_method_tags[MID_STR_LOCALE]) return jni_make_string("en_US");
  if (mid == &g_method_tags[MID_STR_API]) return jni_make_string("14");
  if (mid == &g_method_tags[MID_STR_RAM]) return jni_make_string("512");
  if (mid == &g_method_tags[MID_STR_VERSION]) return jni_make_string("1200");
  if (mid == &g_method_tags[MID_STR_GENERIC]) return string_for_generic();
  return jni_make_string("");
}

static void *call_object_common(void *obj, void *mid, va_list ap) {
  if (mid == &g_method_tags[MID_ASSET_OPEN]) {
    void *s = va_arg(ap, void *);
    return stream_open(jstr(s));
  }
  if (mid == &g_method_tags[MID_ASSET_LIST]) {
    void *s = va_arg(ap, void *);
    return asset_list(jstr(s));
  }
  if (mid == &g_method_tags[MID_ASSET_OPENFD]) {
    void *s = va_arg(ap, void *);
    return afd_open(jstr(s));
  }
  if (mid == &g_method_tags[MID_GET_ASSETS]) return jni_asset_manager();
  if (mid == &g_method_tags[MID_GET_INSTANCE]) return jni_activity_object();
  if (mid == &g_method_tags[MID_STR_GENERIC] || mid == &g_method_tags[MID_STR_ZERO] ||
      mid == &g_method_tags[MID_STR_ONE] || mid == &g_method_tags[MID_STR_APPDIR] ||
      mid == &g_method_tags[MID_STR_EXTDIR] || mid == &g_method_tags[MID_STR_LANGUAGE] ||
      mid == &g_method_tags[MID_STR_LOCALE] || mid == &g_method_tags[MID_STR_API] ||
      mid == &g_method_tags[MID_STR_RAM] || mid == &g_method_tags[MID_STR_VERSION])
    return string_for_tag(mid);
  (void)obj;
  return jni_make_string("");
}

static void *jni_CallObjectMethod(void *env, void *obj, void *mid, ...) {
  (void)env;
  va_list ap;
  va_start(ap, mid);
  void *r = call_object_common(obj, mid, ap);
  va_end(ap);
  return r;
}
static void *jni_CallObjectMethodV(void *env, void *obj, void *mid, va_list ap) {
  (void)env;
  return call_object_common(obj, mid, ap);
}
static void *jni_CallObjectMethodA(void *env, void *obj, void *mid, const void *args) {
  (void)env;
  (void)obj;
  const uintptr_t *a = (const uintptr_t *)args;
  if (mid == &g_method_tags[MID_ASSET_OPEN]) return stream_open(jstr((void *)a[0]));
  if (mid == &g_method_tags[MID_ASSET_LIST]) return asset_list(jstr((void *)a[0]));
  if (mid == &g_method_tags[MID_ASSET_OPENFD]) return afd_open(jstr((void *)a[0]));
  if (mid == &g_method_tags[MID_GET_INSTANCE]) return jni_activity_object();
  if (mid >= &g_method_tags[MID_STR_GENERIC] && mid <= &g_method_tags[MID_STR_VERSION])
    return string_for_tag(mid);
  return jni_make_string("");
}

static void *jni_CallStaticObjectMethod(void *env, void *clazz, void *mid, ...) {
  (void)env;
  (void)clazz;
  if (mid == &g_method_tags[MID_GET_INSTANCE]) return jni_activity_object();
  if (mid >= &g_method_tags[MID_STR_GENERIC] && mid <= &g_method_tags[MID_STR_VERSION])
    return string_for_tag(mid);
  return string_for_generic();
}
static void *jni_CallStaticObjectMethodV(void *env, void *clazz, void *mid, va_list ap) {
  (void)ap;
  return jni_CallStaticObjectMethod(env, clazz, mid);
}
static void *jni_CallStaticObjectMethodA(void *env, void *clazz, void *mid, const void *args) {
  (void)args;
  return jni_CallStaticObjectMethod(env, clazz, mid);
}

static jint stream_read(JStream *st, JArray *arr, int off, int len) {
  if (!st || st->magic != MAGIC_STREAM || !st->fp || !arr || arr->magic != MAGIC_ARRAY || arr->kind != 1)
    return -1;
  if (off < 0) off = 0;
  if (len < 0) len = 0;
  if (off > arr->len) return -1;
  if (off + len > arr->len) len = arr->len - off;
  if (len <= 0) return 0;
  size_t n = fread(arr->data + off, 1, (size_t)len, st->fp);
  if (n == 0 && feof(st->fp)) return -1;
  return (jint)n;
}

static jint audio_write_array(JArray *arr, int off, int len) {
  if (!arr || arr->magic != MAGIC_ARRAY || arr->kind != 2 || !arr->data) return 0;
  if (off < 0) off = 0;
  if (len < 0) len = 0;
  if (off + len > arr->len) len = arr->len - off;
  if (len <= 0) return 0;
  if (g_audio_write_cb) g_audio_write_cb((const int16_t *)arr->data + off, len);
  if (getenv("DS_AUDIOLOG")) {
    const int16_t *pcm = (const int16_t *)arr->data + off;
    int peak = 0;
    unsigned long long sum_abs = 0;
    for (int i = 0; i < len; i++) {
      int v = pcm[i];
      int a = v < 0 ? -v : v;
      if (a > peak) peak = a;
      sum_abs += (unsigned)a;
    }
    unsigned n = __sync_add_and_fetch(&g_audio_write_calls, 1);
    if (n <= 96 || (n % 256) == 0)
      fprintf(stderr, "[audio] write samples=%d off=%d peak=%d avg_abs=%llu call=%u\n",
              len, off, peak, len > 0 ? sum_abs / (unsigned)len : 0, n);
  }
  return len;
}

static jint jni_CallIntMethod(void *env, void *obj, void *mid, ...) {
  (void)env;
  va_list ap;
  va_start(ap, mid);
  jint r = 0;
  if (mid == &g_method_tags[MID_INPUT_READ1]) {
    JArray *arr = va_arg(ap, JArray *);
    r = stream_read((JStream *)obj, arr, 0, arr ? arr->len : 0);
  } else if (mid == &g_method_tags[MID_INPUT_READ3]) {
    JArray *arr = va_arg(ap, JArray *);
    int off = va_arg(ap, int);
    int len = va_arg(ap, int);
    r = stream_read((JStream *)obj, arr, off, len);
  } else if (mid == &g_method_tags[MID_INPUT_AVAILABLE]) {
    JStream *st = (JStream *)obj;
    if (st && st->magic == MAGIC_STREAM && st->fp) r = (jint)(st->len - ftell(st->fp));
  } else if (mid == &g_method_tags[MID_AUDIO_WRITE]) {
    JArray *arr = va_arg(ap, JArray *);
    int off = va_arg(ap, int);
    int len = va_arg(ap, int);
    r = audio_write_array(arr, off, len);
  } else if (mid == &g_method_tags[MID_INT_WIDTH]) {
    r = g_screen_w;
  } else if (mid == &g_method_tags[MID_INT_HEIGHT]) {
    r = g_screen_h;
  } else if (mid == &g_method_tags[MID_INT_ORIENTATION]) {
    r = 0;
  }
  va_end(ap);
  return r;
}
static jint jni_CallIntMethodV(void *env, void *obj, void *mid, va_list ap) {
  (void)env;
  if (mid == &g_method_tags[MID_INT_WIDTH]) return g_screen_w;
  if (mid == &g_method_tags[MID_INT_HEIGHT]) return g_screen_h;
  if (mid == &g_method_tags[MID_INT_ORIENTATION]) return 0;
  if (mid == &g_method_tags[MID_INPUT_READ1]) {
    JArray *arr = va_arg(ap, JArray *);
    return stream_read((JStream *)obj, arr, 0, arr ? arr->len : 0);
  }
  if (mid == &g_method_tags[MID_AUDIO_WRITE]) {
    JArray *arr = va_arg(ap, JArray *);
    int off = va_arg(ap, int);
    int len = va_arg(ap, int);
    return audio_write_array(arr, off, len);
  }
  return 0;
}
static jint jni_CallIntMethodA(void *env, void *obj, void *mid, const void *args) {
  (void)env;
  const uintptr_t *a = (const uintptr_t *)args;
  if (mid == &g_method_tags[MID_INT_WIDTH]) return g_screen_w;
  if (mid == &g_method_tags[MID_INT_HEIGHT]) return g_screen_h;
  if (mid == &g_method_tags[MID_INT_ORIENTATION]) return 0;
  if (mid == &g_method_tags[MID_INPUT_READ1]) {
    JArray *arr = (JArray *)a[0];
    return stream_read((JStream *)obj, arr, 0, arr ? arr->len : 0);
  }
  if (mid == &g_method_tags[MID_AUDIO_WRITE]) {
    JArray *arr = (JArray *)a[0];
    int off = (int)a[1];
    int len = (int)a[2];
    return audio_write_array(arr, off, len);
  }
  return 0;
}

static jlong jni_CallLongMethod(void *env, void *obj, void *mid, ...) {
  (void)env;
  va_list ap;
  va_start(ap, mid);
  jlong r = 0;
  if (mid == &g_method_tags[MID_LONG_MEMORY]) {
    r = 512LL * 1024LL * 1024LL;
  } else if (mid == &g_method_tags[MID_INPUT_SKIP]) {
    jlong want = va_arg(ap, jlong);
    JStream *st = (JStream *)obj;
    if (st && st->magic == MAGIC_STREAM && st->fp) {
      long cur = ftell(st->fp);
      if (want < 0) want = 0;
      if (cur + want > st->len) want = st->len - cur;
      fseek(st->fp, (long)want, SEEK_CUR);
      r = want;
    }
  } else if (mid == &g_method_tags[MID_AFD_GET_LENGTH]) {
    JAFD *afd = (JAFD *)obj;
    if (afd && afd->magic == MAGIC_AFD) r = afd->len;
  }
  va_end(ap);
  return r;
}
static jlong jni_CallLongMethodV(void *env, void *obj, void *mid, va_list ap) {
  (void)env;
  if (mid == &g_method_tags[MID_LONG_MEMORY]) return 512LL * 1024LL * 1024LL;
  if (mid == &g_method_tags[MID_INPUT_SKIP]) {
    jlong want = va_arg(ap, jlong);
    JStream *st = (JStream *)obj;
    if (st && st->magic == MAGIC_STREAM && st->fp) {
      long cur = ftell(st->fp);
      if (want < 0) want = 0;
      if (cur + want > st->len) want = st->len - cur;
      fseek(st->fp, (long)want, SEEK_CUR);
      return want;
    }
  }
  return 0;
}
static jlong jni_CallLongMethodA(void *env, void *obj, void *mid, const void *args) {
  (void)env;
  (void)args;
  if (mid == &g_method_tags[MID_LONG_MEMORY]) return 512LL * 1024LL * 1024LL;
  if (mid == &g_method_tags[MID_AFD_GET_LENGTH]) {
    JAFD *afd = (JAFD *)obj;
    return (afd && afd->magic == MAGIC_AFD) ? afd->len : 0;
  }
  return 0;
}

static float jni_CallFloatMethod(void *env, void *obj, void *mid, ...) {
  (void)env;
  (void)obj;
  if (mid == &g_method_tags[MID_FLOAT_DPI]) return 160.0f;
  return 0.0f;
}
static float jni_CallFloatMethodV(void *env, void *obj, void *mid, va_list ap) {
  (void)ap;
  return jni_CallFloatMethod(env, obj, mid);
}
static float jni_CallFloatMethodA(void *env, void *obj, void *mid, const void *args) {
  (void)args;
  return jni_CallFloatMethod(env, obj, mid);
}

static void jni_CallVoidMethod(void *env, void *obj, void *mid, ...) {
  (void)env;
  if (mid == &g_method_tags[MID_INPUT_CLOSE]) stream_close(obj);
  else if (mid == &g_method_tags[MID_AUDIO_PLAY]) {
    if (getenv("DS_AUDIOLOG")) fprintf(stderr, "[audio] AudioTrack.play\n");
    if (g_audio_state_cb) g_audio_state_cb(1);
  }
  else if (mid == &g_method_tags[MID_AUDIO_STOP]) {
    if (getenv("DS_AUDIOLOG")) fprintf(stderr, "[audio] AudioTrack.stop/pause\n");
    if (g_audio_state_cb) g_audio_state_cb(0);
  }
  else if (mid == &g_method_tags[MID_AUDIO_RELEASE]) {
    if (getenv("DS_AUDIOLOG")) fprintf(stderr, "[audio] AudioTrack.release\n");
    if (g_audio_state_cb) g_audio_state_cb(0);
  } else if (mid == &g_method_tags[MID_AUDIO_FLUSH]) {
    if (getenv("DS_AUDIOLOG")) fprintf(stderr, "[audio] AudioTrack.flush\n");
    if (g_audio_state_cb) g_audio_state_cb(2);
  } else if (mid == &g_method_tags[MID_EXIT]) {
    exit(0);
  }
}
static void jni_CallVoidMethodV(void *env, void *obj, void *mid, va_list ap) {
  (void)ap;
  jni_CallVoidMethod(env, obj, mid);
}
static void jni_CallVoidMethodA(void *env, void *obj, void *mid, const void *args) {
  (void)args;
  jni_CallVoidMethod(env, obj, mid);
}

static jboolean jni_CallBooleanMethod(void *env, void *obj, void *mid, ...) {
  (void)env;
  (void)obj;
  (void)mid;
  return 1;
}
static jboolean jni_CallBooleanMethodV(void *env, void *obj, void *mid, va_list ap) {
  (void)ap;
  return jni_CallBooleanMethod(env, obj, mid);
}
static jboolean jni_CallBooleanMethodA(void *env, void *obj, void *mid, const void *args) {
  (void)args;
  return jni_CallBooleanMethod(env, obj, mid);
}

static jint jni_CallStaticIntMethod(void *env, void *clazz, void *mid, ...) {
  (void)env;
  (void)clazz;
  if (mid == &g_method_tags[MID_INT_WIDTH]) return g_screen_w;
  if (mid == &g_method_tags[MID_INT_HEIGHT]) return g_screen_h;
  if (mid == &g_method_tags[MID_INT_ORIENTATION]) return 0;
  return 1;
}
static jint jni_CallStaticIntMethodV(void *env, void *clazz, void *mid, va_list ap) {
  (void)ap;
  return jni_CallStaticIntMethod(env, clazz, mid);
}
static jint jni_CallStaticIntMethodA(void *env, void *clazz, void *mid, const void *args) {
  (void)args;
  return jni_CallStaticIntMethod(env, clazz, mid);
}
static jlong jni_CallStaticLongMethod(void *env, void *clazz, void *mid, ...) {
  (void)env; (void)clazz;
  if (mid == &g_method_tags[MID_LONG_MEMORY]) return 512LL * 1024LL * 1024LL;
  return 0;
}
static jlong jni_CallStaticLongMethodV(void *env, void *clazz, void *mid, va_list ap) {
  (void)ap;
  return jni_CallStaticLongMethod(env, clazz, mid);
}
static jlong jni_CallStaticLongMethodA(void *env, void *clazz, void *mid, const void *args) {
  (void)args;
  return jni_CallStaticLongMethod(env, clazz, mid);
}
static float jni_CallStaticFloatMethod(void *env, void *clazz, void *mid, ...) {
  (void)env; (void)clazz;
  if (mid == &g_method_tags[MID_FLOAT_DPI]) return 160.0f;
  return 0.0f;
}
static jboolean jni_CallStaticBooleanMethod(void *env, void *clazz, void *mid, ...) {
  (void)env; (void)clazz; (void)mid; return 1;
}
static void jni_CallStaticVoidMethod(void *env, void *clazz, void *mid, ...) {
  (void)env; (void)clazz;
  if (mid == &g_method_tags[MID_EXIT]) exit(0);
}

static void *jni_NewStringUTF(void *env, const char *str) {
  (void)env;
  return jni_make_string(str);
}
static jint jni_GetStringUTFLength(void *env, void *s) {
  (void)env;
  return (jint)strlen(jstr(s));
}
static const char *jni_GetStringUTFChars(void *env, void *s, void *is_copy) {
  (void)env;
  if (is_copy) *(jboolean *)is_copy = 0;
  return jstr(s);
}
static void jni_ReleaseStringUTFChars(void *env, void *s, const char *chars) {
  (void)env; (void)s; (void)chars;
}
static jint jni_GetStringLength(void *env, void *s) {
  return jni_GetStringUTFLength(env, s);
}
static unsigned short *jni_GetStringChars(void *env, void *s, void *is_copy) {
  (void)env;
  const char *src = jstr(s);
  size_t n = strlen(src);
  unsigned short *w = calloc(n + 1, sizeof(unsigned short));
  for (size_t i = 0; i < n; i++) w[i] = (unsigned char)src[i];
  if (is_copy) *(jboolean *)is_copy = 1;
  return w;
}
static void jni_ReleaseStringChars(void *env, void *s, unsigned short *chars) {
  (void)env; (void)s; free(chars);
}

static void *jni_NewGlobalRef(void *env, void *obj) { (void)env; return obj; }
static void *jni_NewLocalRef(void *env, void *obj) { (void)env; return obj; }
static void jni_DeleteGlobalRef(void *env, void *obj) { (void)env; (void)obj; }
static void jni_DeleteLocalRef(void *env, void *obj) { (void)env; (void)obj; }
static void *jni_GetObjectClass(void *env, void *obj) {
  (void)env;
  if (obj == &g_asset_manager_obj) return &g_class_asset_manager;
  if (obj == &g_audio_obj) return &g_class_audio;
  if (obj == &g_renderer_obj) return &g_class_renderer;
  return &g_class_activity;
}
static jboolean jni_IsSameObject(void *env, void *a, void *b) { (void)env; return a == b; }

static jint jni_GetArrayLength(void *env, void *array) {
  (void)env;
  JArray *a = (JArray *)array;
  return (a && a->magic == MAGIC_ARRAY) ? a->len : 0;
}
static void *jni_NewObjectArray(void *env, jint len, void *clazz, void *init) {
  (void)env; (void)clazz;
  JArray *a = array_new(3, len);
  for (int i = 0; i < len; i++) a->objects[i] = init;
  return a;
}
static void *jni_GetObjectArrayElement(void *env, void *array, jint index) {
  (void)env;
  JArray *a = (JArray *)array;
  if (!a || a->magic != MAGIC_ARRAY || a->kind != 3 || index < 0 || index >= a->len) return NULL;
  return a->objects[index];
}
static void jni_SetObjectArrayElement(void *env, void *array, jint index, void *value) {
  (void)env;
  JArray *a = (JArray *)array;
  if (a && a->magic == MAGIC_ARRAY && a->kind == 3 && index >= 0 && index < a->len) a->objects[index] = value;
}
static void *jni_NewByteArray(void *env, jint len) { (void)env; return array_new(1, len); }
static void *jni_NewShortArray(void *env, jint len) { (void)env; return array_new(2, len); }
static void *jni_GetByteArrayElements(void *env, void *array, void *is_copy) {
  (void)env;
  if (is_copy) *(jboolean *)is_copy = 0;
  JArray *a = (JArray *)array;
  return (a && a->magic == MAGIC_ARRAY && a->kind == 1) ? a->data : NULL;
}
static void *jni_GetShortArrayElements(void *env, void *array, void *is_copy) {
  (void)env;
  if (is_copy) *(jboolean *)is_copy = 0;
  JArray *a = (JArray *)array;
  return (a && a->magic == MAGIC_ARRAY && a->kind == 2) ? a->data : NULL;
}
static void jni_ReleaseByteArrayElements(void *env, void *array, void *elems, jint mode) {
  (void)env; (void)array; (void)elems; (void)mode;
}
static void jni_ReleaseShortArrayElements(void *env, void *array, void *elems, jint mode) {
  (void)env; (void)array; (void)elems; (void)mode;
}
static void jni_GetByteArrayRegion(void *env, void *array, jint start, jint len, void *buf) {
  (void)env;
  JArray *a = (JArray *)array;
  if (!a || a->magic != MAGIC_ARRAY || a->kind != 1 || !buf) return;
  if (start < 0 || len < 0 || start + len > a->len) return;
  memcpy(buf, a->data + start, len);
}
static void jni_GetShortArrayRegion(void *env, void *array, jint start, jint len, void *buf) {
  (void)env;
  JArray *a = (JArray *)array;
  if (!a || a->magic != MAGIC_ARRAY || a->kind != 2 || !buf) return;
  if (start < 0 || len < 0 || start + len > a->len) return;
  memcpy(buf, (int16_t *)a->data + start, (size_t)len * sizeof(int16_t));
}
static void jni_SetByteArrayRegion(void *env, void *array, jint start, jint len, const void *buf) {
  (void)env;
  JArray *a = (JArray *)array;
  if (!a || a->magic != MAGIC_ARRAY || a->kind != 1 || !buf) return;
  if (start < 0 || len < 0 || start + len > a->len) return;
  memcpy(a->data + start, buf, len);
}
static void jni_SetShortArrayRegion(void *env, void *array, jint start, jint len, const void *buf) {
  (void)env;
  JArray *a = (JArray *)array;
  if (!a || a->magic != MAGIC_ARRAY || a->kind != 2 || !buf) return;
  if (start < 0 || len < 0 || start + len > a->len) return;
  memcpy((int16_t *)a->data + start, buf, (size_t)len * sizeof(int16_t));
}

static jboolean jni_ExceptionCheck(void *env) { (void)env; return 0; }
static void *jni_ExceptionOccurred(void *env) { (void)env; return NULL; }
static void jni_ExceptionDescribe(void *env) { (void)env; }
static void jni_ExceptionClear(void *env) { (void)env; }
static jint jni_GetJavaVM(void *env, void **vm) { (void)env; if (vm) *vm = &java_vm_ptr; return 0; }

static jint vm_DestroyJavaVM(void *vm) { (void)vm; return 0; }
static jint vm_AttachCurrentThread(void *vm, void **penv, void *args) {
  (void)vm; (void)args; if (penv) *penv = &jni_env_ptr; return 0;
}
static jint vm_DetachCurrentThread(void *vm) { (void)vm; return 0; }
static jint vm_GetEnv(void *vm, void **penv, jint version) {
  (void)vm; (void)version; if (penv) *penv = &jni_env_ptr; return 0;
}

void jni_shim_init(void **out_vm, void **out_env) {
  g_jni_log = getenv("DS_JNILOG") != NULL;
  g_asset_log = getenv("DS_ASSETLOG") != NULL;
  for (int i = 0; i < JNI_VTABLE_SIZE; i++) {
    jni_env_vtable[i] = (uintptr_t)jni_stub;
    java_vm_vtable[i] = (uintptr_t)jni_stub;
  }
  jni_env_vtable[4] = (uintptr_t)jni_GetVersion;
  jni_env_vtable[6] = (uintptr_t)jni_FindClass;
  jni_env_vtable[15] = (uintptr_t)jni_ExceptionOccurred;
  jni_env_vtable[16] = (uintptr_t)jni_ExceptionDescribe;
  jni_env_vtable[17] = (uintptr_t)jni_ExceptionClear;
  jni_env_vtable[21] = (uintptr_t)jni_NewGlobalRef;
  jni_env_vtable[22] = (uintptr_t)jni_DeleteGlobalRef;
  jni_env_vtable[23] = (uintptr_t)jni_DeleteLocalRef;
  jni_env_vtable[24] = (uintptr_t)jni_IsSameObject;
  jni_env_vtable[25] = (uintptr_t)jni_NewLocalRef;
  jni_env_vtable[31] = (uintptr_t)jni_GetObjectClass;
  jni_env_vtable[33] = (uintptr_t)jni_GetMethodID;
  jni_env_vtable[34] = (uintptr_t)jni_CallObjectMethod;
  jni_env_vtable[35] = (uintptr_t)jni_CallObjectMethodV;
  jni_env_vtable[36] = (uintptr_t)jni_CallObjectMethodA;
  jni_env_vtable[37] = (uintptr_t)jni_CallBooleanMethod;
  jni_env_vtable[38] = (uintptr_t)jni_CallBooleanMethodV;
  jni_env_vtable[39] = (uintptr_t)jni_CallBooleanMethodA;
  jni_env_vtable[49] = (uintptr_t)jni_CallIntMethod;
  jni_env_vtable[50] = (uintptr_t)jni_CallIntMethodV;
  jni_env_vtable[51] = (uintptr_t)jni_CallIntMethodA;
  jni_env_vtable[52] = (uintptr_t)jni_CallLongMethod;
  jni_env_vtable[53] = (uintptr_t)jni_CallLongMethodV;
  jni_env_vtable[54] = (uintptr_t)jni_CallLongMethodA;
  jni_env_vtable[55] = (uintptr_t)jni_CallFloatMethod;
  jni_env_vtable[56] = (uintptr_t)jni_CallFloatMethodV;
  jni_env_vtable[57] = (uintptr_t)jni_CallFloatMethodA;
  jni_env_vtable[61] = (uintptr_t)jni_CallVoidMethod;
  jni_env_vtable[62] = (uintptr_t)jni_CallVoidMethodV;
  jni_env_vtable[63] = (uintptr_t)jni_CallVoidMethodA;
  jni_env_vtable[113] = (uintptr_t)jni_GetStaticMethodID;
  jni_env_vtable[114] = (uintptr_t)jni_CallStaticObjectMethod;
  jni_env_vtable[115] = (uintptr_t)jni_CallStaticObjectMethodV;
  jni_env_vtable[116] = (uintptr_t)jni_CallStaticObjectMethodA;
  jni_env_vtable[117] = (uintptr_t)jni_CallBooleanMethod;
  jni_env_vtable[118] = (uintptr_t)jni_CallBooleanMethodV;
  jni_env_vtable[119] = (uintptr_t)jni_CallBooleanMethodA;
  jni_env_vtable[129] = (uintptr_t)jni_CallStaticIntMethod;
  jni_env_vtable[130] = (uintptr_t)jni_CallStaticIntMethodV;
  jni_env_vtable[131] = (uintptr_t)jni_CallStaticIntMethodA;
  jni_env_vtable[132] = (uintptr_t)jni_CallStaticLongMethod;
  jni_env_vtable[133] = (uintptr_t)jni_CallStaticLongMethodV;
  jni_env_vtable[134] = (uintptr_t)jni_CallStaticLongMethodA;
  jni_env_vtable[135] = (uintptr_t)jni_CallStaticFloatMethod;
  jni_env_vtable[136] = (uintptr_t)jni_CallStaticFloatMethod;
  jni_env_vtable[137] = (uintptr_t)jni_CallStaticFloatMethod;
  jni_env_vtable[141] = (uintptr_t)jni_CallStaticVoidMethod;
  jni_env_vtable[142] = (uintptr_t)jni_CallStaticVoidMethod;
  jni_env_vtable[143] = (uintptr_t)jni_CallStaticVoidMethod;
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
  jni_env_vtable[178] = (uintptr_t)jni_NewShortArray;
  jni_env_vtable[184] = (uintptr_t)jni_GetByteArrayElements;
  jni_env_vtable[186] = (uintptr_t)jni_GetShortArrayElements;
  jni_env_vtable[192] = (uintptr_t)jni_ReleaseByteArrayElements;
  jni_env_vtable[194] = (uintptr_t)jni_ReleaseShortArrayElements;
  jni_env_vtable[200] = (uintptr_t)jni_GetByteArrayRegion;
  jni_env_vtable[202] = (uintptr_t)jni_GetShortArrayRegion;
  jni_env_vtable[205] = (uintptr_t)jni_ExceptionCheck;
  jni_env_vtable[208] = (uintptr_t)jni_SetByteArrayRegion;
  jni_env_vtable[210] = (uintptr_t)jni_SetShortArrayRegion;
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
