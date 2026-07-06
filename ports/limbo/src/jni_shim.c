#include "jni_shim.h"

#include "util.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define JNI_VTABLE_SIZE 512

typedef int jint;
typedef unsigned char jboolean;
typedef long long jlong;

static uintptr_t jni_env_vtable[JNI_VTABLE_SIZE];
static void *jni_env_ptr;
static uintptr_t java_vm_vtable[JNI_VTABLE_SIZE];
static void *java_vm_ptr;

enum {
  MID_GENERIC = 0,
  MID_GET_APPLICATION_INFO,
  MID_GET_SYSTEM_SERVICE,
  MID_GET_PROPERTY,
  MID_GET_NATIVE_OUTPUT_SAMPLE_RATE,
  MID_DETECT_CONTROLS,
  MID_DUMP_CONTROLS,
  MID_IS_TV_DEVICE,
  MID_ABORT,
  FID_GENERIC,
  FID_NATIVE_LIBRARY_DIR,
  FID_SDK_INT,
  FID_GAMEPAD_DEVICE_ID,
  FID_GAMEPAD_PRODUCT_ID,
  FID_GAMEPAD_VENDOR_ID,
  FID_GAMEPAD_BUTTON_CODES,
  FID_GAMEPAD_AXIS_CODES,
  FID_GAMEPAD_AXIS_MIN_VALS,
  FID_GAMEPAD_AXIS_MAX_VALS,
  FID_GAMEPAD_AXIS_SOURCES,
  FID_BUILD_MANUFACTURER,
  FID_BUILD_MODEL,
  FID_BUILD_BRAND,
  FID_BUILD_DEVICE,
  FID_BUILD_PRODUCT,
  FID_CONTEXT_AUDIO_SERVICE,
  FID_AUDIO_PROPERTY_OUTPUT_FRAMES_PER_BUFFER,
  FID_AUDIO_PROPERTY_OUTPUT_SAMPLE_RATE,
  TAG_COUNT
};

static int g_tags[TAG_COUNT];
static int g_jni_log;
static int g_fake_obj;
static int g_fake_activity;
static int g_fake_app_info;
static int g_fake_audio_manager;
static int g_gamepad_device_id = 1;
static int g_gamepad_vendor_id = 0x045e;
static int g_gamepad_product_id = 0x028e;
static int g_gamepad_button_codes[] = {
    96, 97, 99, 100, 101, 102, 104, 103,
    105, 106, 107, 108, 109, 19, 20, 21, 22, 23,
};
static int g_gamepad_axis_codes[] = {0, 1, 11, 14, 15, 16, 17, 18};
static int g_gamepad_axis_sources[] = {
    0x01000010, 0x01000010, 0x01000010, 0x01000010,
    0x01000010, 0x01000010, 0x01000010, 0x01000010,
};
static float g_gamepad_axis_min_vals[] = {
    -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, 0.0f, 0.0f,
};
static float g_gamepad_axis_max_vals[] = {
    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
};

static int jni_reported_sdk_version(void) {
  const char *env = getenv("LIMBO_ANDROID_SDK");
  if (env && *env) {
    int sdk = atoi(env);
    if (sdk >= 14 && sdk <= 100)
      return sdk;
  }
  return 23;
}

#define MAX_JSTRINGS 256
static struct {
  void *handle;
  const char *value;
  int owned;
} g_jstrings[MAX_JSTRINGS];
static int g_jstring_count;

void *jni_make_string(const char *value) {
  if (g_jstring_count >= MAX_JSTRINGS)
    g_jstring_count = 0;
  int idx = g_jstring_count++;
  if (g_jstrings[idx].owned)
    free((void *)g_jstrings[idx].value);
  g_jstrings[idx].handle = (void *)((uintptr_t)0x10000 + (uintptr_t)idx);
  g_jstrings[idx].value = value ? value : "";
  g_jstrings[idx].owned = 0;
  return g_jstrings[idx].handle;
}

static void *jni_make_owned_string(const char *value) {
  char *copy = strdup(value ? value : "");
  void *h = jni_make_string(copy ? copy : "");
  int idx = (int)((uintptr_t)h - 0x10000);
  if (idx >= 0 && idx < MAX_JSTRINGS)
    g_jstrings[idx].owned = copy != NULL;
  return h;
}

static const char *resolve_jstring(void *jstr) {
  for (int i = 0; i < g_jstring_count; i++) {
    if (g_jstrings[i].handle == jstr)
      return g_jstrings[i].value;
  }
  return "";
}

static intptr_t jni_stub(void) { return 0; }
static jint jni_GetVersion(void *env) { (void)env; return 0x00010006; }

static void *jni_FindClass(void *env, const char *name) {
  (void)env;
  if (g_jni_log)
    debugPrintf("JNI FindClass(%s)\n", name ? name : "(null)");
  return &g_fake_obj;
}

static int tag_for_method(const char *name) {
  if (!name)
    return MID_GENERIC;
  if (!strcmp(name, "getApplicationInfo"))
    return MID_GET_APPLICATION_INFO;
  if (!strcmp(name, "getSystemService"))
    return MID_GET_SYSTEM_SERVICE;
  if (!strcmp(name, "getProperty"))
    return MID_GET_PROPERTY;
  if (!strcmp(name, "getNativeOutputSampleRate"))
    return MID_GET_NATIVE_OUTPUT_SAMPLE_RATE;
  if (!strcmp(name, "DetectControls"))
    return MID_DETECT_CONTROLS;
  if (!strcmp(name, "DumpControls"))
    return MID_DUMP_CONTROLS;
  if (!strcmp(name, "IsTVDevice"))
    return MID_IS_TV_DEVICE;
  if (!strcmp(name, "Abort"))
    return MID_ABORT;
  return MID_GENERIC;
}

static int tag_for_field(const char *name) {
  if (!name)
    return FID_GENERIC;
  if (!strcmp(name, "nativeLibraryDir"))
    return FID_NATIVE_LIBRARY_DIR;
  if (!strcmp(name, "SDK_INT"))
    return FID_SDK_INT;
  if (!strcmp(name, "MANUFACTURER"))
    return FID_BUILD_MANUFACTURER;
  if (!strcmp(name, "MODEL"))
    return FID_BUILD_MODEL;
  if (!strcmp(name, "BRAND"))
    return FID_BUILD_BRAND;
  if (!strcmp(name, "DEVICE"))
    return FID_BUILD_DEVICE;
  if (!strcmp(name, "PRODUCT"))
    return FID_BUILD_PRODUCT;
  if (!strcmp(name, "AUDIO_SERVICE"))
    return FID_CONTEXT_AUDIO_SERVICE;
  if (!strcmp(name, "PROPERTY_OUTPUT_FRAMES_PER_BUFFER"))
    return FID_AUDIO_PROPERTY_OUTPUT_FRAMES_PER_BUFFER;
  if (!strcmp(name, "PROPERTY_OUTPUT_SAMPLE_RATE"))
    return FID_AUDIO_PROPERTY_OUTPUT_SAMPLE_RATE;
  if (!strcmp(name, "gamepadDeviceId"))
    return FID_GAMEPAD_DEVICE_ID;
  if (!strcmp(name, "gamepadVendorId"))
    return FID_GAMEPAD_VENDOR_ID;
  if (!strcmp(name, "gamepadProductId"))
    return FID_GAMEPAD_PRODUCT_ID;
  if (!strcmp(name, "gamepadButtonCodes"))
    return FID_GAMEPAD_BUTTON_CODES;
  if (!strcmp(name, "gamepadAxisCodes"))
    return FID_GAMEPAD_AXIS_CODES;
  if (!strcmp(name, "gamepadAxisMinVals"))
    return FID_GAMEPAD_AXIS_MIN_VALS;
  if (!strcmp(name, "gamepadAxisMaxVals"))
    return FID_GAMEPAD_AXIS_MAX_VALS;
  if (!strcmp(name, "gamepadAxisSources"))
    return FID_GAMEPAD_AXIS_SOURCES;
  return FID_GENERIC;
}

static void *jni_GetMethodID(void *env, void *clazz, const char *name,
                             const char *sig) {
  (void)env; (void)clazz;
  if (g_jni_log)
    debugPrintf("JNI GetMethodID(%s, %s)\n", name ? name : "?", sig ? sig : "?");
  return &g_tags[tag_for_method(name)];
}

static void *jni_GetStaticMethodID(void *env, void *clazz, const char *name,
                                   const char *sig) {
  (void)env; (void)clazz;
  if (g_jni_log)
    debugPrintf("JNI GetStaticMethodID(%s, %s)\n", name ? name : "?", sig ? sig : "?");
  return &g_tags[tag_for_method(name)];
}

static void *jni_GetFieldID(void *env, void *clazz, const char *name,
                            const char *sig) {
  (void)env; (void)clazz;
  if (g_jni_log)
    debugPrintf("JNI GetFieldID(%s, %s)\n", name ? name : "?", sig ? sig : "?");
  return &g_tags[tag_for_field(name)];
}

static void *jni_GetStaticFieldID(void *env, void *clazz, const char *name,
                                  const char *sig) {
  (void)env; (void)clazz;
  if (g_jni_log)
    debugPrintf("JNI GetStaticFieldID(%s, %s)\n", name ? name : "?", sig ? sig : "?");
  return &g_tags[tag_for_field(name)];
}

static void *jni_CallObjectMethod(void *env, void *obj, void *mid, ...) {
  (void)env; (void)obj;
  if (mid == &g_tags[MID_GET_APPLICATION_INFO])
    return &g_fake_app_info;
  if (mid == &g_tags[MID_GET_SYSTEM_SERVICE])
    return &g_fake_audio_manager;
  if (mid == &g_tags[MID_GET_PROPERTY]) {
    va_list ap;
    va_start(ap, mid);
    void *key = va_arg(ap, void *);
    va_end(ap);
    const char *k = resolve_jstring(key);
    if (strstr(k, "sample_rate") || strstr(k, "OUTPUT_SAMPLE_RATE"))
      return jni_make_string("48000");
    if (strstr(k, "frames_per_buffer") ||
        strstr(k, "OUTPUT_FRAMES_PER_BUFFER"))
      return jni_make_string("512");
    return jni_make_string("");
  }
  return &g_fake_obj;
}

static void *jni_CallStaticObjectMethod(void *env, void *clazz, void *mid, ...) {
  (void)env; (void)clazz;
  return jni_CallObjectMethod(env, clazz, mid);
}

static void *jni_CallStaticObjectMethodV(void *env, void *clazz, void *mid,
                                         va_list ap) {
  (void)ap;
  return jni_CallStaticObjectMethod(env, clazz, mid);
}

static jint jni_CallIntMethod(void *env, void *obj, void *mid, ...) {
  (void)env; (void)obj;
  if (mid == &g_tags[MID_GET_NATIVE_OUTPUT_SAMPLE_RATE])
    return 48000;
  return 0;
}

static jint jni_CallStaticIntMethod(void *env, void *clazz, void *mid, ...) {
  return jni_CallIntMethod(env, clazz, mid);
}

static jboolean jni_CallBooleanMethod(void *env, void *obj, void *mid, ...) {
  (void)env; (void)obj;
  if (mid == &g_tags[MID_IS_TV_DEVICE]) {
    if (g_jni_log)
      debugPrintf("JNI IsTVDevice -> true\n");
    return 1;
  }
  return 0;
}

static jboolean jni_CallStaticBooleanMethod(void *env, void *clazz, void *mid, ...) {
  (void)env; (void)clazz; (void)mid;
  return 0;
}

static void jni_CallVoidMethod(void *env, void *obj, void *mid, ...) {
  (void)env; (void)obj;
  if (mid == &g_tags[MID_DETECT_CONTROLS]) {
    if (g_jni_log)
      debugPrintf("JNI DetectControls -> virtual gamepad ready\n");
    return;
  }
  if (mid == &g_tags[MID_DUMP_CONTROLS]) {
    if (g_jni_log)
      debugPrintf("JNI DumpControls: buttons=%zu axes=%zu device=%d\n",
                  sizeof(g_gamepad_button_codes) /
                      sizeof(g_gamepad_button_codes[0]),
                  sizeof(g_gamepad_axis_codes) /
                      sizeof(g_gamepad_axis_codes[0]),
                  g_gamepad_device_id);
    return;
  }
  if (mid == &g_tags[MID_ABORT]) {
    debugPrintf("JNI Abort requested\n");
    _exit(0);
  }
}

static void jni_CallStaticVoidMethod(void *env, void *clazz, void *mid, ...) {
  jni_CallVoidMethod(env, clazz, mid);
}

static void *jni_GetObjectField(void *env, void *obj, void *fid) {
  (void)env; (void)obj;
  if (fid == &g_tags[FID_NATIVE_LIBRARY_DIR])
    return jni_make_string(".");
  if (fid == &g_tags[FID_GAMEPAD_BUTTON_CODES])
    return g_gamepad_button_codes;
  if (fid == &g_tags[FID_GAMEPAD_AXIS_CODES])
    return g_gamepad_axis_codes;
  if (fid == &g_tags[FID_GAMEPAD_AXIS_MIN_VALS])
    return g_gamepad_axis_min_vals;
  if (fid == &g_tags[FID_GAMEPAD_AXIS_MAX_VALS])
    return g_gamepad_axis_max_vals;
  if (fid == &g_tags[FID_GAMEPAD_AXIS_SOURCES])
    return g_gamepad_axis_sources;
  return &g_fake_obj;
}

static void *jni_GetStaticObjectField(void *env, void *clazz, void *fid) {
  (void)env; (void)clazz;
  if (g_jni_log)
    debugPrintf("JNI GetStaticObjectField(fid=%p)\n", fid);
  if (fid == &g_tags[FID_BUILD_MANUFACTURER])
    return jni_make_string("NextOS");
  if (fid == &g_tags[FID_BUILD_MODEL])
    return jni_make_string("NextOS");
  if (fid == &g_tags[FID_BUILD_BRAND])
    return jni_make_string("NextOS");
  if (fid == &g_tags[FID_BUILD_DEVICE])
    return jni_make_string("nextos");
  if (fid == &g_tags[FID_BUILD_PRODUCT])
    return jni_make_string("nextos");
  if (fid == &g_tags[FID_CONTEXT_AUDIO_SERVICE])
    return jni_make_string("audio");
  if (fid == &g_tags[FID_AUDIO_PROPERTY_OUTPUT_FRAMES_PER_BUFFER])
    return jni_make_string("android.media.property.OUTPUT_FRAMES_PER_BUFFER");
  if (fid == &g_tags[FID_AUDIO_PROPERTY_OUTPUT_SAMPLE_RATE])
    return jni_make_string("android.media.property.OUTPUT_SAMPLE_RATE");
  return jni_make_string("");
}

static jint jni_GetIntField(void *env, void *obj, void *fid) {
  (void)env; (void)obj;
  if (fid == &g_tags[FID_GAMEPAD_DEVICE_ID])
    return g_gamepad_device_id;
  if (fid == &g_tags[FID_GAMEPAD_VENDOR_ID])
    return g_gamepad_vendor_id;
  if (fid == &g_tags[FID_GAMEPAD_PRODUCT_ID])
    return g_gamepad_product_id;
  return 0;
}

static jint jni_GetStaticIntField(void *env, void *clazz, void *fid) {
  (void)env; (void)clazz;
  if (fid == &g_tags[FID_SDK_INT])
    return jni_reported_sdk_version();
  return 0;
}

static void *jni_NewStringUTF(void *env, const char *str) {
  (void)env;
  return jni_make_owned_string(str ? str : "");
}

static jint jni_GetStringUTFLength(void *env, void *jstr) {
  (void)env;
  return (jint)strlen(resolve_jstring(jstr));
}

static const char *jni_GetStringUTFChars(void *env, void *jstr, void *isCopy) {
  (void)env;
  if (isCopy)
    *(jboolean *)isCopy = 0;
  return resolve_jstring(jstr);
}

static void jni_ReleaseStringUTFChars(void *env, void *jstr, const char *chars) {
  (void)env; (void)jstr; (void)chars;
}

static jint jni_GetStringLength(void *env, void *jstr) {
  return jni_GetStringUTFLength(env, jstr);
}

static unsigned short *jni_GetStringChars(void *env, void *jstr, void *isCopy) {
  (void)env;
  const char *s = resolve_jstring(jstr);
  size_t n = strlen(s);
  unsigned short *buf = calloc(n + 1, sizeof(unsigned short));
  if (!buf)
    return NULL;
  for (size_t i = 0; i < n; i++)
    buf[i] = (unsigned char)s[i];
  if (isCopy)
    *(jboolean *)isCopy = 1;
  return buf;
}

static void jni_ReleaseStringChars(void *env, void *jstr, unsigned short *chars) {
  (void)env; (void)jstr;
  free(chars);
}

static void *jni_NewGlobalRef(void *env, void *obj) { (void)env; return obj; }
static void *jni_NewLocalRef(void *env, void *obj) { (void)env; return obj; }
static void jni_DeleteGlobalRef(void *env, void *obj) { (void)env; (void)obj; }
static void jni_DeleteLocalRef(void *env, void *obj) { (void)env; (void)obj; }
static void *jni_GetObjectClass(void *env, void *obj) { (void)env; (void)obj; return &g_fake_obj; }
static jboolean jni_ExceptionCheck(void *env) { (void)env; return 0; }
static void jni_ExceptionClear(void *env) { (void)env; }
static void *jni_ExceptionOccurred(void *env) { (void)env; return NULL; }
static void jni_ExceptionDescribe(void *env) { (void)env; }

static jint jni_GetArrayLength(void *env, void *array) {
  (void)env;
  if (array == g_gamepad_button_codes)
    return (jint)(sizeof(g_gamepad_button_codes) /
                  sizeof(g_gamepad_button_codes[0]));
  if (array == g_gamepad_axis_codes || array == g_gamepad_axis_sources)
    return (jint)(sizeof(g_gamepad_axis_codes) /
                  sizeof(g_gamepad_axis_codes[0]));
  if (array == g_gamepad_axis_min_vals || array == g_gamepad_axis_max_vals)
    return (jint)(sizeof(g_gamepad_axis_min_vals) /
                  sizeof(g_gamepad_axis_min_vals[0]));
  return 0;
}

static void *jni_GetIntArrayElements(void *env, void *array, void *isCopy) {
  (void)env;
  if (isCopy)
    *(jboolean *)isCopy = 0;
  if (array == g_gamepad_button_codes)
    return g_gamepad_button_codes;
  if (array == g_gamepad_axis_codes)
    return g_gamepad_axis_codes;
  if (array == g_gamepad_axis_sources)
    return g_gamepad_axis_sources;
  return NULL;
}

static void *jni_GetFloatArrayElements(void *env, void *array, void *isCopy) {
  (void)env;
  if (isCopy)
    *(jboolean *)isCopy = 0;
  if (array == g_gamepad_axis_min_vals)
    return g_gamepad_axis_min_vals;
  if (array == g_gamepad_axis_max_vals)
    return g_gamepad_axis_max_vals;
  return NULL;
}

static void jni_GetIntArrayRegion(void *env, void *array, jint start,
                                  jint len, jint *buf) {
  (void)env;
  const int *src = NULL;
  jint count = jni_GetArrayLength(env, array);
  if (array == g_gamepad_button_codes)
    src = g_gamepad_button_codes;
  else if (array == g_gamepad_axis_codes)
    src = g_gamepad_axis_codes;
  else if (array == g_gamepad_axis_sources)
    src = g_gamepad_axis_sources;
  if (!src || !buf || start < 0 || len < 0 || start > count)
    return;
  if (start + len > count)
    len = count - start;
  memcpy(buf, src + start, (size_t)len * sizeof(*buf));
}

static void jni_GetFloatArrayRegion(void *env, void *array, jint start,
                                    jint len, float *buf) {
  (void)env;
  const float *src = NULL;
  jint count = jni_GetArrayLength(env, array);
  if (array == g_gamepad_axis_min_vals)
    src = g_gamepad_axis_min_vals;
  else if (array == g_gamepad_axis_max_vals)
    src = g_gamepad_axis_max_vals;
  if (!src || !buf || start < 0 || len < 0 || start > count)
    return;
  if (start + len > count)
    len = count - start;
  memcpy(buf, src + start, (size_t)len * sizeof(*buf));
}

static void jni_ReleaseArrayElements(void *env, void *array, void *elems, jint mode) {
  (void)env; (void)array; (void)elems; (void)mode;
}

static jint vm_DestroyJavaVM(void *vm) { (void)vm; return 0; }
static jint vm_AttachCurrentThread(void *vm, void **penv, void *args) {
  (void)vm; (void)args;
  if (penv)
    *penv = &jni_env_ptr;
  return 0;
}
static jint vm_DetachCurrentThread(void *vm) { (void)vm; return 0; }
static jint vm_GetEnv(void *vm, void **penv, jint version) {
  (void)vm; (void)version;
  if (penv)
    *penv = &jni_env_ptr;
  return 0;
}

void jni_shim_init(void **out_vm, void **out_env) {
  g_jni_log = getenv("LIMBO_JNILOG") != NULL;
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
  jni_env_vtable[25] = (uintptr_t)jni_NewLocalRef;
  jni_env_vtable[31] = (uintptr_t)jni_GetObjectClass;
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
  jni_env_vtable[94] = (uintptr_t)jni_GetFieldID;
  jni_env_vtable[95] = (uintptr_t)jni_GetObjectField;
  jni_env_vtable[100] = (uintptr_t)jni_GetIntField;
  jni_env_vtable[113] = (uintptr_t)jni_GetStaticMethodID;
  jni_env_vtable[114] = (uintptr_t)jni_CallStaticObjectMethod;
  jni_env_vtable[115] = (uintptr_t)jni_CallStaticObjectMethodV;
  jni_env_vtable[116] = (uintptr_t)jni_CallStaticObjectMethod;
  jni_env_vtable[117] = (uintptr_t)jni_CallStaticBooleanMethod;
  jni_env_vtable[118] = (uintptr_t)jni_CallStaticBooleanMethod;
  jni_env_vtable[119] = (uintptr_t)jni_CallStaticBooleanMethod;
  jni_env_vtable[129] = (uintptr_t)jni_CallStaticIntMethod;
  jni_env_vtable[130] = (uintptr_t)jni_CallStaticIntMethod;
  jni_env_vtable[131] = (uintptr_t)jni_CallStaticIntMethod;
  jni_env_vtable[141] = (uintptr_t)jni_CallStaticVoidMethod;
  jni_env_vtable[142] = (uintptr_t)jni_CallStaticVoidMethod;
  jni_env_vtable[143] = (uintptr_t)jni_CallStaticVoidMethod;
  jni_env_vtable[144] = (uintptr_t)jni_GetStaticFieldID;
  jni_env_vtable[145] = (uintptr_t)jni_GetStaticObjectField;
  jni_env_vtable[150] = (uintptr_t)jni_GetStaticIntField;
  jni_env_vtable[164] = (uintptr_t)jni_GetStringLength;
  jni_env_vtable[165] = (uintptr_t)jni_GetStringChars;
  jni_env_vtable[166] = (uintptr_t)jni_ReleaseStringChars;
  jni_env_vtable[167] = (uintptr_t)jni_NewStringUTF;
  jni_env_vtable[168] = (uintptr_t)jni_GetStringUTFLength;
  jni_env_vtable[169] = (uintptr_t)jni_GetStringUTFChars;
  jni_env_vtable[170] = (uintptr_t)jni_ReleaseStringUTFChars;
  jni_env_vtable[171] = (uintptr_t)jni_GetArrayLength;
  jni_env_vtable[187] = (uintptr_t)jni_GetIntArrayElements;
  jni_env_vtable[189] = (uintptr_t)jni_GetFloatArrayElements;
  jni_env_vtable[190] = (uintptr_t)jni_GetFloatArrayElements;
  jni_env_vtable[203] = (uintptr_t)jni_GetIntArrayRegion;
  jni_env_vtable[205] = (uintptr_t)jni_GetFloatArrayRegion;
  jni_env_vtable[195] = (uintptr_t)jni_ReleaseArrayElements;
  jni_env_vtable[197] = (uintptr_t)jni_ReleaseArrayElements;
  jni_env_vtable[198] = (uintptr_t)jni_ReleaseArrayElements;
  jni_env_vtable[228] = (uintptr_t)jni_ExceptionCheck;

  java_vm_vtable[3] = (uintptr_t)vm_DestroyJavaVM;
  java_vm_vtable[4] = (uintptr_t)vm_AttachCurrentThread;
  java_vm_vtable[5] = (uintptr_t)vm_DetachCurrentThread;
  java_vm_vtable[6] = (uintptr_t)vm_GetEnv;

  jni_env_ptr = jni_env_vtable;
  java_vm_ptr = java_vm_vtable;
  if (out_vm)
    *out_vm = &java_vm_ptr;
  if (out_env)
    *out_env = &jni_env_ptr;

  debugPrintf("JNI virtual gamepad: device=%d vendor=0x%x product=0x%x "
              "buttons=%zu axes=%zu\n",
              g_gamepad_device_id, g_gamepad_vendor_id,
              g_gamepad_product_id,
              sizeof(g_gamepad_button_codes) /
                  sizeof(g_gamepad_button_codes[0]),
              sizeof(g_gamepad_axis_codes) / sizeof(g_gamepad_axis_codes[0]));
}
