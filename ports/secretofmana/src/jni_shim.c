/*
 * jni_shim.c -- JNIEnv falso para o engine plandroid (Secret of Mana / MCF).
 *
 * O engine (libplandroid) chama de volta no "Java" (classe PlAndroidLib) via
 * as funcoes nativas JNI_PlAndroidLib_*, que fazem FindClass + GetStaticMethodID
 * + CallStatic*Method. Interceptamos por NOME do metodo e devolvemos valores
 * saos / executamos a acao nativamente:
 *   - GetSensorStateFunc([I)  -> preenche o int[37] com o input (g_som_input)
 *   - GetLanguage/GetDeviceLanguage -> ingles
 *   - GetObbMountedPath/GetAppVersionName -> strings
 *   - Sound / Music -> stub (audio implementado depois)
 *   - Font*Func -> metricas aproximadas / draw no-op (texto do sistema)
 * Assets vem do disco via AAsset shim (imports.c); AssetManager e' dummy.
 */
#include "jni_shim.h"
#include "so_util.h"
#include "text_render.h"
#include "util.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define JNI_VTABLE_SIZE 512

typedef int jint;
typedef unsigned char jboolean;

som_input_t g_som_input;
int g_som_lang = 1; /* default ingles (ajustavel via SOM_LANG) */

static uintptr_t jni_env_vtable[JNI_VTABLE_SIZE];
static void *jni_env_ptr;
static uintptr_t java_vm_vtable[JNI_VTABLE_SIZE];
static void *java_vm_ptr;
static int g_jni_log = 0;

/* ---- jstrings falsas ---- */
#define MAX_JSTRINGS 256
static struct { void *handle; const char *value; } g_jstrings[MAX_JSTRINGS];
static int g_jstring_count = 0;
void *jni_make_string(const char *value) {
  if (g_jstring_count >= MAX_JSTRINGS) g_jstring_count = 0;
  int idx = g_jstring_count++;
  g_jstrings[idx].handle = (void *)((uintptr_t)0x10000 + idx);
  g_jstrings[idx].value = value ? value : "";
  return g_jstrings[idx].handle;
}
static const char *resolve_jstring(void *jstr) {
  for (int i = 0; i < g_jstring_count; i++)
    if (g_jstrings[i].handle == jstr) return g_jstrings[i].value;
  return "";
}

/* ---- arrays (byte[] e int[]) ---- data guardada em bytes ---- */
#define MAX_JARRAYS 64
static struct { void *handle; unsigned char *data; int len; int owned; } g_jarr[MAX_JARRAYS];
static int g_jarr_n = 0;
static void *jarr_new(int bytelen, int owned, unsigned char *existing) {
  int idx = -1;
  for (int i = 0; i < g_jarr_n; i++) if (!g_jarr[i].handle) { idx = i; break; }
  if (idx < 0) { if (g_jarr_n >= MAX_JARRAYS) g_jarr_n = 0; idx = g_jarr_n++; }
  g_jarr[idx].data = existing ? existing : (bytelen > 0 ? calloc(bytelen, 1) : NULL);
  g_jarr[idx].len = bytelen;
  g_jarr[idx].owned = owned;
  g_jarr[idx].handle = (void *)((uintptr_t)0x20000 + idx);
  return g_jarr[idx].handle;
}
static int jarr_find(void *h) {
  for (int i = 0; i < g_jarr_n; i++) if (g_jarr[i].handle == h) return i;
  return -1;
}

/* ---- tags de metodos (por nome) ---- */
enum {
  MID_GENERIC = 0,
  MID_GET_SENSOR_STATE,
  MID_GET_LANGUAGE, MID_GET_DEVICE_LANGUAGE,
  MID_GET_OBB_PATH, MID_APP_VER_NAME, MID_APP_VER_CODE,
  MID_IS_TV, MID_IS_RETINA, MID_IS_BLEND_OES,
  MID_DEV_TOTAL_MEM, MID_DEV_FREE_MEM, MID_DEV_MAX_MEM, MID_NAT_TOTAL_MEM, MID_NAT_FREE_MEM,
  MID_SOUND_LOAD, MID_MUSIC_LOAD,
  MID_FONT_WIDTH, MID_FONT_HEIGHT, MID_FONT_DRAW,
  MID_TAG_COUNT
};
static int g_method_tags[MID_TAG_COUNT];

static int tag_for_method(const char *name) {
  if (!name) return MID_GENERIC;
  if (strstr(name, "GetSensorState")) return MID_GET_SENSOR_STATE;
  if (strcmp(name, "GetDeviceLanguage") == 0) return MID_GET_DEVICE_LANGUAGE;
  if (strcmp(name, "GetLanguage") == 0) return MID_GET_LANGUAGE;
  if (strstr(name, "GetObbMountedPath")) return MID_GET_OBB_PATH;
  if (strstr(name, "GetAppVersionName")) return MID_APP_VER_NAME;
  if (strstr(name, "GetAppVersionCode")) return MID_APP_VER_CODE;
  if (strstr(name, "IsDeviceAndroidTV")) return MID_IS_TV;
  if (strstr(name, "isRetina")) return MID_IS_RETINA;
  if (strstr(name, "IsGLESBlendEquationOES")) return MID_IS_BLEND_OES;
  if (strstr(name, "DeviceTotalMemory")) return MID_DEV_TOTAL_MEM;
  if (strstr(name, "DeviceFreeMemory")) return MID_DEV_FREE_MEM;
  if (strstr(name, "DeviceMaxMemory")) return MID_DEV_MAX_MEM;
  if (strstr(name, "NativeTotalMemory")) return MID_NAT_TOTAL_MEM;
  if (strstr(name, "NativeFreeMemory")) return MID_NAT_FREE_MEM;
  if (strstr(name, "SoundLoad")) return MID_SOUND_LOAD;
  if (strstr(name, "MusicLoad")) return MID_MUSIC_LOAD;
  if (strstr(name, "FontWidth")) return MID_FONT_WIDTH;
  if (strstr(name, "FontHeight")) return MID_FONT_HEIGHT;
  if (strstr(name, "FontDrawString")) return MID_FONT_DRAW;
  return MID_GENERIC;
}

static intptr_t jni_stub(void) { return 0; }
static jint jni_GetVersion(void *env) { return 0x00010006; }
static void *jni_FindClass(void *env, const char *name) {
  if (g_jni_log) debugPrintf("JNI FindClass(%s)\n", name);
  static int fake_class; return &fake_class;
}
static void *jni_GetMethodID(void *env, void *clazz, const char *name, const char *sig) {
  if (g_jni_log) debugPrintf("JNI GetMethodID(%s, %s)\n", name, sig);
  return &g_method_tags[tag_for_method(name)];
}
static void *jni_GetStaticMethodID(void *env, void *clazz, const char *name, const char *sig) {
  if (g_jni_log) debugPrintf("JNI GetStaticMethodID(%s, %s)\n", name, sig);
  return &g_method_tags[tag_for_method(name)];
}
static void *jni_GetFieldID(void *env, void *clazz, const char *name, const char *sig) {
  return &g_method_tags[MID_GENERIC];
}
static void *jni_GetStaticFieldID(void *env, void *clazz, const char *name, const char *sig) {
  return &g_method_tags[MID_GENERIC];
}

/* preenche o int[37] do PlAndroidSensor a partir de g_som_input */
static void fill_sensor_array(void *arrHandle) {
  int i = jarr_find(arrHandle);
  if (i < 0 || !g_jarr[i].data) return;
  int *a = (int *)g_jarr[i].data;
  int n = g_jarr[i].len / 4;
  if (n < 37) return;
  som_input_t *s = &g_som_input;
  a[0] = s->key_now;  a[1] = s->key_last; a[2] = s->key_on; a[3] = s->key_off;
  a[4] = s->touch_ptr_max; a[5] = s->touch_count; a[6] = s->touch_last_ptr;
  a[7] = s->touch_now_b; a[8] = s->touch_last_b; a[9] = s->touch_on_b;
  a[10] = s->touch_off_b; a[11] = s->touch_moving_b; a[12] = s->touch_move_b;
  a[13] = s->touch_max_x; a[14] = s->touch_max_y;
  for (int k = 0; k < 4; k++) { a[15+k] = s->touch_start_x[k]; a[19+k] = s->touch_start_y[k];
                                a[23+k] = s->touch_move_x[k];  a[27+k] = s->touch_move_y[k]; }
  a[31] = s->analog_x[0]; a[32] = s->analog_x[1];
  a[33] = s->analog_y[0]; a[34] = s->analog_y[1];
  a[35] = 0; a[36] = 0;
}

/* ---- retornos por tag ---- */
static jint int_for_tag(void *mid, void *firstArg) {
  static int snd_id = 1, mus_id = 1;
  if (mid == &g_method_tags[MID_GET_LANGUAGE] ||
      mid == &g_method_tags[MID_GET_DEVICE_LANGUAGE]) return g_som_lang;
  if (mid == &g_method_tags[MID_APP_VER_CODE]) return 201906071;
  if (mid == &g_method_tags[MID_IS_TV]) return 0;
  if (mid == &g_method_tags[MID_IS_RETINA]) return 0;
  if (mid == &g_method_tags[MID_IS_BLEND_OES]) return 1;
  if (mid == &g_method_tags[MID_DEV_TOTAL_MEM]) return 512 * 1024;
  if (mid == &g_method_tags[MID_DEV_FREE_MEM]) return 256 * 1024;
  if (mid == &g_method_tags[MID_DEV_MAX_MEM]) return 512 * 1024;
  if (mid == &g_method_tags[MID_NAT_TOTAL_MEM]) return 256 * 1024;
  if (mid == &g_method_tags[MID_NAT_FREE_MEM]) return 192 * 1024;
  if (mid == &g_method_tags[MID_SOUND_LOAD]) return snd_id++;
  if (mid == &g_method_tags[MID_MUSIC_LOAD]) return mus_id++;
  return 0;
}

/* FontWidth/Height: args (int type, int size, String). */
static jint font_metric(void *mid, int size, void *strArg) {
  if (mid == &g_method_tags[MID_FONT_HEIGHT]) return som_text_height(size);
  if (mid == &g_method_tags[MID_FONT_WIDTH]) return som_text_width(resolve_jstring(strArg), size);
  return 0;
}

/* FontDraw: args (int[] buf, w, h, _, size, x, y, r, g, b, a, String). */
static void font_draw(void *arr, int w, int h, int size, int x, int y,
                      int r, int g, int b, void *strArg) {
  int i = jarr_find(arr);
  if (i < 0 || !g_jarr[i].data) return;
  if ((long)w * h * 4 > g_jarr[i].len) return;
  som_text_draw(resolve_jstring(strArg), size, (unsigned int *)g_jarr[i].data,
                w, h, x, y, r, g, b);
}
static void *obj_for_tag(void *mid) {
  if (mid == &g_method_tags[MID_GET_OBB_PATH]) {
    const char *e = getenv("SOM_OBB_PATH");
    return jni_make_string(e ? e : "./assets");
  }
  if (mid == &g_method_tags[MID_APP_VER_NAME]) return jni_make_string("3.4.3");
  return jni_make_string("");
}

/* ---- Call*Method (instancia) ---- */
static void *jni_CallObjectMethod(void *env, void *obj, void *mid, ...) { return obj_for_tag(mid); }
static jint jni_CallIntMethod(void *env, void *obj, void *mid, ...) { return int_for_tag(mid, NULL); }
static jboolean jni_CallBooleanMethod(void *env, void *obj, void *mid, ...) { return 0; }
static void jni_CallVoidMethod(void *env, void *obj, void *mid, ...) {}

/* ---- CallStatic*Method (variadic ...) ---- */
static void *jni_CallStaticObjectMethod(void *env, void *clazz, void *mid, ...) { return obj_for_tag(mid); }
static void *jni_CallStaticObjectMethodV(void *env, void *clazz, void *mid, va_list a) { return obj_for_tag(mid); }
static void *jni_CallStaticObjectMethodA(void *env, void *clazz, void *mid, const void *a) { return obj_for_tag(mid); }

static int is_font_metric(void *mid) {
  return mid == &g_method_tags[MID_FONT_WIDTH] || mid == &g_method_tags[MID_FONT_HEIGHT];
}
static jint jni_CallStaticIntMethod(void *env, void *clazz, void *mid, ...) {
  va_list ap; va_start(ap, mid);
  if (is_font_metric(mid)) {
    int type = va_arg(ap, int); (void)type;
    int size = va_arg(ap, int); void *str = va_arg(ap, void *);
    va_end(ap); return font_metric(mid, size, str);
  }
  void *a0 = va_arg(ap, void *); va_end(ap);
  return int_for_tag(mid, a0);
}
static jint jni_CallStaticIntMethodV(void *env, void *clazz, void *mid, va_list ap) {
  if (is_font_metric(mid)) {
    int type = va_arg(ap, int); (void)type;
    int size = va_arg(ap, int); void *str = va_arg(ap, void *);
    return font_metric(mid, size, str);
  }
  void *a0 = va_arg(ap, void *); return int_for_tag(mid, a0);
}
static jint jni_CallStaticIntMethodA(void *env, void *clazz, void *mid, const void *args) {
  const uint64_t *a = (const uint64_t *)args;
  if (is_font_metric(mid)) return font_metric(mid, (int)a[1], (void *)a[2]);
  return int_for_tag(mid, (void *)a[0]);
}

static void font_draw_va(void *mid, va_list ap) {
  void *arr = va_arg(ap, void *);
  int w = va_arg(ap, int), h = va_arg(ap, int);
  int p20 = va_arg(ap, int); (void)p20;
  int size = va_arg(ap, int), x = va_arg(ap, int), y = va_arg(ap, int);
  int r = va_arg(ap, int), g = va_arg(ap, int), b = va_arg(ap, int);
  int a = va_arg(ap, int); (void)a;
  void *str = va_arg(ap, void *);
  font_draw(arr, w, h, size, x, y, r, g, b, str);
}
static void do_static_void(void *mid, void *firstArg) {
  if (mid == &g_method_tags[MID_GET_SENSOR_STATE]) fill_sensor_array(firstArg);
  /* Sound/Music/demais -> no-op por enquanto */
}
static void jni_CallStaticVoidMethod(void *env, void *clazz, void *mid, ...) {
  va_list ap; va_start(ap, mid);
  if (mid == &g_method_tags[MID_FONT_DRAW]) { font_draw_va(mid, ap); va_end(ap); return; }
  void *a0 = va_arg(ap, void *); va_end(ap);
  do_static_void(mid, a0);
}
static void jni_CallStaticVoidMethodV(void *env, void *clazz, void *mid, va_list ap) {
  if (mid == &g_method_tags[MID_FONT_DRAW]) { font_draw_va(mid, ap); return; }
  void *a0 = va_arg(ap, void *); do_static_void(mid, a0);
}
static void jni_CallStaticVoidMethodA(void *env, void *clazz, void *mid, const void *args) {
  const uint64_t *a = (const uint64_t *)args;
  if (mid == &g_method_tags[MID_FONT_DRAW]) {
    font_draw((void *)a[0], (int)a[1], (int)a[2], (int)a[4], (int)a[5], (int)a[6],
              (int)a[7], (int)a[8], (int)a[9], (void *)a[11]);
    return;
  }
  do_static_void(mid, (void *)a[0]);
}
static jboolean jni_CallStaticBooleanMethod(void *env, void *clazz, void *mid, ...) { return 0; }
static jboolean jni_CallStaticBooleanMethodV(void *env, void *clazz, void *mid, va_list a) { return 0; }
static jboolean jni_CallStaticBooleanMethodA(void *env, void *clazz, void *mid, const void *a) { return 0; }

/* ---- strings ---- */
static void *jni_NewStringUTF(void *env, const char *str) { return jni_make_string(str ? strdup(str) : ""); }
static jint jni_GetStringUTFLength(void *env, void *jstr) { return (jint)strlen(resolve_jstring(jstr)); }
static const char *jni_GetStringUTFChars(void *env, void *jstr, void *isCopy) {
  if (isCopy) *(unsigned char *)isCopy = 0;
  return resolve_jstring(jstr);
}
static void jni_ReleaseStringUTFChars(void *env, void *jstr, const char *chars) {}
static jint jni_GetStringLength(void *env, void *jstr) { return (jint)strlen(resolve_jstring(jstr)); }
static unsigned short *jni_GetStringChars(void *env, void *jstr, void *isCopy) {
  const char *s = resolve_jstring(jstr); size_t n = strlen(s);
  unsigned short *buf = malloc((n + 1) * 2);
  for (size_t i = 0; i < n; i++) buf[i] = (unsigned char)s[i];
  buf[n] = 0; if (isCopy) *(unsigned char *)isCopy = 1; return buf;
}
static void jni_ReleaseStringChars(void *env, void *jstr, unsigned short *chars) { free(chars); }

/* ---- refs / classes ---- */
static void *jni_NewGlobalRef(void *env, void *obj) { return obj; }
static void *jni_NewLocalRef(void *env, void *obj) { return obj; }
static void jni_DeleteGlobalRef(void *env, void *obj) {}
static void jni_DeleteLocalRef(void *env, void *obj) {}
static void *jni_GetObjectClass(void *env, void *obj) { static int f; return &f; }
static jboolean jni_IsInstanceOf(void *env, void *obj, void *clazz) { return 1; }

/* ---- arrays ---- */
static jint jni_GetArrayLength(void *env, void *array) {
  int i = jarr_find(array); if (i >= 0) return g_jarr[i].len / 4; return 0;
}
static void *jni_NewByteArray(void *env, jint len) { return jarr_new(len, 1, NULL); }
static void *jni_NewIntArray(void *env, jint len) { return jarr_new(len * 4, 1, NULL); }
static void *jni_GetArrayElements(void *env, void *array, void *isCopy) {
  if (isCopy) *(unsigned char *)isCopy = 0;
  int i = jarr_find(array); if (i >= 0) return g_jarr[i].data; return NULL;
}
static void jni_ReleaseArrayElements(void *env, void *array, void *elems, jint mode) {}
static void jni_GetByteArrayRegion(void *env, void *array, jint start, jint len, void *buf) {
  int i = jarr_find(array); if (i < 0 || !g_jarr[i].data) return;
  if (start < 0 || start + len > g_jarr[i].len) return;
  memcpy(buf, g_jarr[i].data + start, len);
}
static void jni_SetByteArrayRegion(void *env, void *array, jint start, jint len, const void *buf) {
  int i = jarr_find(array); if (i < 0 || !g_jarr[i].data) return;
  if (start < 0 || start + len > g_jarr[i].len) return;
  memcpy(g_jarr[i].data + start, buf, len);
}
static void jni_GetIntArrayRegion(void *env, void *array, jint start, jint len, void *buf) {
  int i = jarr_find(array); if (i < 0 || !g_jarr[i].data) return;
  if (start < 0 || (start + len) * 4 > g_jarr[i].len) return;
  memcpy(buf, g_jarr[i].data + start * 4, len * 4);
}
static void jni_SetIntArrayRegion(void *env, void *array, jint start, jint len, const void *buf) {
  int i = jarr_find(array); if (i < 0 || !g_jarr[i].data) return;
  if (start < 0 || (start + len) * 4 > g_jarr[i].len) return;
  memcpy(g_jarr[i].data + start * 4, buf, len * 4);
}
static void *jni_GetObjectArrayElement(void *env, void *array, jint i) { return jni_make_string("."); }

/* ---- excecoes ---- */
static jboolean jni_ExceptionCheck(void *env) { return 0; }
static void jni_ExceptionClear(void *env) {}
static void *jni_ExceptionOccurred(void *env) { return NULL; }
static void jni_ExceptionDescribe(void *env) {}
static jint jni_Throw(void *env, void *obj) { return 0; }
static jint jni_ThrowNew(void *env, void *clazz, const char *msg) { return 0; }

/* ---- JavaVM ---- */
static jint vm_DestroyJavaVM(void *vm) { return 0; }
static jint vm_AttachCurrentThread(void *vm, void **penv, void *args) { if (penv) *penv = &jni_env_ptr; return 0; }
static jint vm_DetachCurrentThread(void *vm) { return 0; }
static jint vm_GetEnv(void *vm, void **penv, jint version) { if (penv) *penv = &jni_env_ptr; return 0; }

void *AAssetManager_fromJava(void *env, void *assetManager) { return (void *)0x1337; }

void jni_shim_init(void **out_vm, void **out_env) {
  g_jni_log = getenv("SOM_JNILOG") != NULL;
  for (int i = 0; i < JNI_VTABLE_SIZE; i++) {
    jni_env_vtable[i] = (uintptr_t)jni_stub;
    java_vm_vtable[i] = (uintptr_t)jni_stub;
  }
  jni_env_vtable[4]  = (uintptr_t)jni_GetVersion;
  jni_env_vtable[6]  = (uintptr_t)jni_FindClass;
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
  jni_env_vtable[34] = (uintptr_t)jni_CallObjectMethod;
  jni_env_vtable[35] = (uintptr_t)jni_CallObjectMethod;
  jni_env_vtable[36] = (uintptr_t)jni_CallObjectMethod;
  jni_env_vtable[37] = (uintptr_t)jni_CallBooleanMethod;
  jni_env_vtable[38] = (uintptr_t)jni_CallBooleanMethod;
  jni_env_vtable[39] = (uintptr_t)jni_CallBooleanMethod;
  jni_env_vtable[49] = (uintptr_t)jni_CallIntMethod;
  jni_env_vtable[50] = (uintptr_t)jni_CallIntMethod;
  jni_env_vtable[51] = (uintptr_t)jni_CallIntMethod;
  jni_env_vtable[61] = (uintptr_t)jni_CallVoidMethod;
  jni_env_vtable[62] = (uintptr_t)jni_CallVoidMethod;
  jni_env_vtable[63] = (uintptr_t)jni_CallVoidMethod;
  jni_env_vtable[106] = (uintptr_t)jni_GetFieldID;
  jni_env_vtable[111] = (uintptr_t)jni_GetStaticFieldID;
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
  jni_env_vtable[173] = (uintptr_t)jni_GetObjectArrayElement;
  jni_env_vtable[176] = (uintptr_t)jni_NewByteArray;
  jni_env_vtable[179] = (uintptr_t)jni_NewIntArray;
  jni_env_vtable[184] = (uintptr_t)jni_GetArrayElements; /* GetByteArrayElements */
  jni_env_vtable[187] = (uintptr_t)jni_GetArrayElements; /* GetIntArrayElements */
  jni_env_vtable[192] = (uintptr_t)jni_ReleaseArrayElements; /* ReleaseByteArrayElements */
  jni_env_vtable[195] = (uintptr_t)jni_ReleaseArrayElements; /* ReleaseIntArrayElements */
  jni_env_vtable[200] = (uintptr_t)jni_GetByteArrayRegion;
  jni_env_vtable[203] = (uintptr_t)jni_GetIntArrayRegion;
  jni_env_vtable[205] = (uintptr_t)jni_ExceptionCheck;
  jni_env_vtable[208] = (uintptr_t)jni_SetByteArrayRegion;
  jni_env_vtable[211] = (uintptr_t)jni_SetIntArrayRegion;

  java_vm_vtable[3] = (uintptr_t)vm_DestroyJavaVM;
  java_vm_vtable[4] = (uintptr_t)vm_AttachCurrentThread;
  java_vm_vtable[5] = (uintptr_t)vm_DetachCurrentThread;
  java_vm_vtable[6] = (uintptr_t)vm_GetEnv;

  jni_env_ptr = jni_env_vtable;
  java_vm_ptr = java_vm_vtable;
  if (out_vm) *out_vm = &java_vm_ptr;
  if (out_env) *out_env = &jni_env_ptr;
}
