/*
 * jni_shim.c -- fake JNI environment for Syberia
 *
 * Android JNI works through double-indirection:
 *   JavaVM *vm;   vm->GetEnv(vm, &env, version)
 *   JNIEnv *env;  env->FindClass(env, "com/foo/Bar")
 *
 * Both vm and env are pointers to a pointer to a function table.
 * We create large stub vtables that return 0/NULL for everything,
 * with specific overrides for methods Syberia actually uses.
 */

#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "egl_shim.h"
#include "jni_shim.h"
#include "so_util.h"
#include "util.h"

#define JNI_VTABLE_SIZE 512
#define JNI_SF __attribute__((pcs("aapcs")))

typedef int jint;
typedef union {
  unsigned char z;
  signed char b;
  unsigned short c;
  short s;
  jint i;
  long long j;
  float f;
  double d;
  void *l;
} jvalue;

static uintptr_t jni_env_vtable[JNI_VTABLE_SIZE];
static void *jni_env_ptr;

static uintptr_t java_vm_vtable[JNI_VTABLE_SIZE];
static void *java_vm_ptr;

/* ---- Tagged method/field IDs ---- */
enum {
  MID_UNKNOWN = 0,
  MID_GET_STORAGE_DIR,
  MID_GET_PACK_NAME,
  MID_SET_ACTIVITY,
  MID_ERROR_DIALOG,
  MID_GET_CLASS_LOADER,
  MID_LOAD_CLASS,
  MID_AUDIO_STREAM_PARAMS,
  MID_BATTERY_LEVEL,
  MID_BATTERY_STATUS,
  MID_GET_DEFAULT_LOCALE,
  MID_GET_LANGUAGE,
  MID_GET_COUNTRY,
  MID_GET_PREFERENCE_STRING,
  MID_GET_PACKAGE,
  MID_GET_SAVE_FOLDER,
  MID_GET_CONTEXT,
  MID_GET_USER_AGENT,
  MID_GET_RESOURCE,
  MID_SETUP_PATHS,
  MID_CREATE_VIEW,
  MID_SET_VIEW_SETTINGS,
  MID_SET_CURRENT_CONTEXT,
  MID_CHECK_HDMI_STATE,
  MID_GET_HDMI_NAME,
  MID_GET_UDID,
  MID_GET_HIDFV,
  MID_GET_ANDROID_ID,
  MID_GET_WINDOW_WIDTH,
  MID_GET_WINDOW_HEIGHT,
  MID_GET_RESOLUTION_X,
  MID_GET_RESOLUTION_Y,
  MID_LAUNCH_VIDEO_PLAYER,
  MID_GET_KEYBOARD_TEXT,
  MID_IS_KEYBOARD_VISIBLE,
  MID_GET_MANUFACTURER,
  MID_GET_DEVICE_NAME,
  MID_GET_DEVICE_FIRMWARE,
  MID_GET_CPU_MAX,
  MID_GET_CPU_CURRENT,
  MID_GET_MAX_RAM,
  MID_GET_FREE_DISK,
  MID_GET_FREE_RAM,
  MID_GET_DEVICE_LANGUAGE,
  MID_GET_DEVICE_WIDTH,
  MID_GET_DEVICE_HEIGHT,
  MID_GET_UPTIME,
  MID_GET_GAME_NAME,
  MID_GET_INJECTED_IGP,
  MID_GET_INJECTED_SERIAL_KEY,
  MID_GET_SD_FOLDER,
  MID_GET_APK_PATH,
  MID_GET_ASSET_AS_STRING,
  MID_GET_META_DATA_VALUE,
  MID_INIT_CHECK_CONNECTION,
  MID_RETRIEVE_BARRELS,
  MID_GET_GLUID,
  MID_D1,
  MID_GET_SERIAL,
  MID_GET_SERIAL_NO,
  MID_GET_DEVICE_FIRMWARE_JAVA,
  MID_GET_MAC_ADDRESS,
  MID_GET_DEVICE_IMEI,
  MID_GET_HIDFV_VERSION,
  MID_GET_GOOGLE_AD_ID,
  MID_GET_GOOGLE_AD_ID_STATUS,
  MID_GET_GLDID,
  MID_GET_DEVICE_NAME_JAVA,
  MID_GET_PHONE_MANUFACTURER,
  MID_GET_PHONE_MODEL,
  MID_RETRIEVE_DEVICE_CARRIER,
  MID_RETRIEVE_DEVICE_COUNTRY,
  MID_RETRIEVE_DEVICE_REGION,
  MID_RETRIEVE_DEVICE_LANGUAGE,
  MID_RETRIEVE_CPU_SERIAL,
  MID_GET_PHONE_DEVICE,
  MID_GET_PHONE_PRODUCT,
  MID_SET_SHARED_VALUE,
  MID_GET_SHARED_VALUE,
  MID_DELETE_SHARED_VALUE,
  MID_IS_SHARED_VALUE,
  MID_GET_MILLISECONDS,
  MID_IAP_GET_DATA,
  MID_BUNDLE_CTOR,
  MID_BUNDLE_CLEAR,
  MID_BUNDLE_CONTAINS_KEY,
  MID_BUNDLE_GET_STRING,
  MID_BUNDLE_GET_INT,
  MID_BUNDLE_GET_LONG,
  MID_BUNDLE_GET_BOOLEAN,
  MID_BUNDLE_GET_BYTE_ARRAY,
  MID_BUNDLE_PUT_STRING,
  MID_BUNDLE_PUT_INT,
  MID_BUNDLE_PUT_LONG,
  MID_BUNDLE_PUT_BOOLEAN,
  MID_BUNDLE_PUT_BYTE_ARRAY,
  MID_BUNDLE_PUT_ALL,
  MID_NATIVE_SET_PREFERENCE,
  MID_NATIVE_GET_PREFERENCE,
  MID_GENERIC,
  FID_OBB_VERSIONCODE,
  FID_BUILD_MANUFACTURER,
  FID_BUILD_MODEL,
  FID_BUILD_DEVICE,
  FID_BUILD_PRODUCT,
  FID_BUILD_VERSION_RELEASE,
  FID_GENERIC,
  TAG_COUNT,
};

#define MAX_KNOWN_IDS 160
static int g_method_tags[MAX_KNOWN_IDS]; /* unique addresses used as method/field IDs */
static char g_id_names[MAX_KNOWN_IDS][96];
static char g_id_sigs[MAX_KNOWN_IDS][160];
static char g_id_kinds[MAX_KNOWN_IDS][24];
extern int dys_screen_w, dys_screen_h;

static _Thread_local int g_static_args_override;
static _Thread_local void *g_static_args[4];
static _Thread_local long long g_static_long_args[4];
static _Thread_local uintptr_t g_jni_callsite_ret;

static void clear_static_args_override(void) {
  g_static_args_override = 0;
  memset(g_static_args, 0, sizeof(g_static_args));
  memset(g_static_long_args, 0, sizeof(g_static_long_args));
}

#define MAX_DYNAMIC_IDS 96
static int g_dynamic_id_tags[MAX_DYNAMIC_IDS];
static char g_dynamic_id_names[MAX_DYNAMIC_IDS][96];
static char g_dynamic_id_sigs[MAX_DYNAMIC_IDS][96];
static char g_dynamic_id_kinds[MAX_DYNAMIC_IDS][24];
static int g_dynamic_id_count;

static int jni_trace_enabled(void) {
  static int enabled = -1;
  if (enabled < 0)
    enabled = getenv("TASM2_JNI_DEBUG") ? 1 : 0;
  return enabled;
}

static void jni_trace(const char *fmt, ...) {
  if (!jni_trace_enabled())
    return;
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
}

static void log_null_method_stack(const char *call) {
  static int dumps;
  if (!jni_trace_enabled() || dumps >= 12 || !text_base)
    return;
  dumps++;

  uintptr_t sp;
  __asm__ volatile("mov %0, sp" : "=r"(sp));
  uintptr_t text = (uintptr_t)text_base;
  fprintf(stderr, "jni_shim: %s mid=NULL stack scan #%d\n", call, dumps);
  int n = 0;
  for (uintptr_t a = sp; a < sp + 0x1800 && n < 24; a += 4) {
    uintptr_t v = *(uintptr_t *)a;
    uintptr_t arm = v & ~(uintptr_t)1;
    if (arm >= text && arm < text + text_size) {
      fprintf(stderr, "jni_shim:   stack[+0x%lx] libtasm2.so+0x%lx raw=%p\n",
              (unsigned long)(a - sp), (unsigned long)(arm - text),
              (void *)v);
      n++;
    }
  }
}

static void copy_text(char *dst, size_t dst_size, const char *src) {
  if (!dst_size)
    return;
  snprintf(dst, dst_size, "%s", src ? src : "");
}

static void *remember_known_id(int tag, const char *kind, const char *name,
                               const char *sig) {
  if (tag >= 0 && tag < (int)(sizeof(g_method_tags) / sizeof(g_method_tags[0]))) {
    copy_text(g_id_names[tag], sizeof(g_id_names[tag]), name ? name : "?");
    copy_text(g_id_sigs[tag], sizeof(g_id_sigs[tag]), sig ? sig : "");
    copy_text(g_id_kinds[tag], sizeof(g_id_kinds[tag]), kind ? kind : "id");
    jni_trace("jni_shim: id %s %s%s -> %p\n", g_id_kinds[tag],
              g_id_names[tag], g_id_sigs[tag], &g_method_tags[tag]);
    return &g_method_tags[tag];
  }
  return NULL;
}

static void *remember_dynamic_id(const char *kind, const char *name,
                                 const char *sig) {
  int idx = g_dynamic_id_count++ % MAX_DYNAMIC_IDS;
  copy_text(g_dynamic_id_names[idx], sizeof(g_dynamic_id_names[idx]),
            name ? name : "?");
  copy_text(g_dynamic_id_sigs[idx], sizeof(g_dynamic_id_sigs[idx]),
            sig ? sig : "");
  copy_text(g_dynamic_id_kinds[idx], sizeof(g_dynamic_id_kinds[idx]),
            kind ? kind : "id");
  jni_trace("jni_shim: id %s %s%s -> %p\n", g_dynamic_id_kinds[idx],
            g_dynamic_id_names[idx], g_dynamic_id_sigs[idx],
            &g_dynamic_id_tags[idx]);
  return &g_dynamic_id_tags[idx];
}

static int known_id_index(void *id) {
  for (int i = 0; i < TAG_COUNT; i++) {
    if (id == &g_method_tags[i])
      return i;
  }
  return -1;
}

static const char *id_name(void *id) {
  int known = known_id_index(id);
  if (known >= 0)
    return g_id_names[known][0] ? g_id_names[known] : "?";
  for (int i = 0; i < MAX_DYNAMIC_IDS; i++) {
    if (id == &g_dynamic_id_tags[i])
      return g_dynamic_id_names[i][0] ? g_dynamic_id_names[i] : "?";
  }
  return "?";
}

static const char *id_sig(void *id) {
  int known = known_id_index(id);
  if (known >= 0)
    return g_id_sigs[known];
  for (int i = 0; i < MAX_DYNAMIC_IDS; i++) {
    if (id == &g_dynamic_id_tags[i])
      return g_dynamic_id_sigs[i];
  }
  return "";
}

static const char *id_kind(void *id) {
  int known = known_id_index(id);
  if (known >= 0)
    return g_id_kinds[known][0] ? g_id_kinds[known] : "id";
  for (int i = 0; i < MAX_DYNAMIC_IDS; i++) {
    if (id == &g_dynamic_id_tags[i])
      return g_dynamic_id_kinds[i][0] ? g_dynamic_id_kinds[i] : "id";
  }
  return "id";
}

static void log_call_id_at(const char *call, void *id, uintptr_t ret) {
  if (!jni_trace_enabled())
    return;
  uintptr_t ret_arm = ret & ~(uintptr_t)1;
  uintptr_t text = (uintptr_t)text_base;
  fprintf(stderr, "jni_shim: %s(%s %s%s, mid=%p)", call, id_kind(id),
          id_name(id), id_sig(id), id);
  if ((!id || getenv("TASM2_JNI_CALLER_DEBUG")) && text_base &&
      ret_arm >= text && ret_arm < text + text_size)
    fprintf(stderr, " caller=libtasm2.so+0x%lx",
            (unsigned long)(ret_arm - text));
  fprintf(stderr, "\n");
  if (!id)
    log_null_method_stack(call);
}

static void log_call_id(const char *call, void *id) {
  log_call_id_at(call, id, g_jni_callsite_ret);
}

#define LOG_STATIC_CALL(call, id)                                            \
  log_call_id_at((call), (id),                                               \
                 g_jni_callsite_ret ? g_jni_callsite_ret                    \
                                    : (uintptr_t)__builtin_return_address(0))

/* ---- Fake class tracking ---- */
#define MAX_FAKE_CLASSES 64
static struct {
  void *handle;
  char name[160];
} g_fake_classes[MAX_FAKE_CLASSES];
static char g_fake_class_handles[MAX_FAKE_CLASSES];
static int g_fake_class_count;

static void *remember_fake_class(const char *name) {
  const char *class_name = name ? name : "";
  for (int i = 0; i < g_fake_class_count; i++) {
    if (strcmp(g_fake_classes[i].name, class_name) == 0)
      return g_fake_classes[i].handle;
  }
  int idx = g_fake_class_count++ % MAX_FAKE_CLASSES;
  g_fake_classes[idx].handle = &g_fake_class_handles[idx];
  copy_text(g_fake_classes[idx].name, sizeof(g_fake_classes[idx].name),
            class_name);
  return g_fake_classes[idx].handle;
}

static const char *resolve_fake_class(void *clazz) {
  if (!clazz)
    return "";
  for (int i = 0; i < MAX_FAKE_CLASSES; i++) {
    if (g_fake_classes[i].handle == clazz)
      return g_fake_classes[i].name;
  }
  return "";
}

static jni_shim_setpaths_fn g_setpaths_callback;
static int g_gameapi_native_init_done;
static int g_gameapi_native_callback_done;

void jni_shim_set_setpaths_callback(jni_shim_setpaths_fn fn) {
  g_setpaths_callback = fn;
}

/* ---- Configurable package/OBB ---- */
static const char *g_package_name = "com.microids.syberia";
static int g_obb_version = 12;

void jni_shim_set_package(const char *package_name, int obb_version) {
  g_package_name = package_name;
  g_obb_version = obb_version;
}

/* ---- Fake jstring tracking ---- */
/* We return tagged pointers as jstrings and map them to C strings */
#define MAX_JSTRINGS 256
static struct {
  void *handle;
  const char *value;
} g_jstrings[MAX_JSTRINGS];
static char g_jstring_values[MAX_JSTRINGS][256];
static int g_jstring_count = 0;

static void *make_jstring(const char *value) {
  static char jstring_storage[MAX_JSTRINGS];
  if (g_jstring_count >= MAX_JSTRINGS)
    g_jstring_count = 0; /* wrap around */
  int idx = g_jstring_count++;
  g_jstrings[idx].handle = &jstring_storage[idx];
  strncpy(g_jstring_values[idx], value ? value : "", sizeof(g_jstring_values[idx]) - 1);
  g_jstring_values[idx][sizeof(g_jstring_values[idx]) - 1] = '\0';
  g_jstrings[idx].value = g_jstring_values[idx];
  return g_jstrings[idx].handle;
}

static const char *resolve_jstring(void *jstr) {
  if (!jstr)
    return "";
  for (int i = 0; i < MAX_JSTRINGS; i++) {
    if (g_jstrings[i].handle == jstr)
      return g_jstrings[i].value;
  }
  return "";
}

void *jni_shim_make_string(const char *value) {
  return make_jstring(value ? value : "");
}

/* ---- SharedValue/DataSharing fake store ---- */
#define MAX_SHARED_VALUES 32
static struct {
  int used;
  char key[128];
  char value[256];
} g_shared_values[MAX_SHARED_VALUES];

static int shared_value_find(const char *key) {
  if (!key || !*key)
    return -1;
  for (int i = 0; i < MAX_SHARED_VALUES; i++) {
    if (g_shared_values[i].used && strcmp(g_shared_values[i].key, key) == 0)
      return i;
  }
  return -1;
}

static const char *shared_value_default(const char *key) {
  if (!key)
    return NULL;
  if (getenv("TASM2_GAIA_NO_DEFAULTS") && strstr(key, "_GAIA_"))
    return NULL;
  if (strstr(key, "_GAIA_ANON_GLUID")) {
    const char *v = getenv("TASM2_FAKE_STORED_GLUID");
    if (v && *v)
      return v;
    return "{\"data\":\"/H3UK6NGclnbMbKIqKpPeQ==\",\"gen\":2,\"password\":\"eDd3Zzk3UGltTGhoSnZYMA==\",\"pck_name\":\"ANMP.GloftASHM\",\"time\":\"1538585115\",\"ver\":2}\n";
  }
  if (strstr(key, "_GAIA_ENC_KEY_GLUID")) {
    const char *v = getenv("TASM2_FAKE_STORED_GLUID_KEY");
    if (v && *v)
      return v;
    return "{\"data\":\"/H3UK6NGclnbMbKIqKpPeQ==\",\"gen\":2,\"password\":\"eDd3Zzk3UGltTGhoSnZYMA==\",\"pck_name\":\"ANMP.GloftASHM\",\"time\":\"1538585115\",\"ver\":2}\n";
  }
  if (strstr(key, "_GAIA_FIRST_LAUNCH")) {
    if (getenv("TASM2_GAIA_FIRST_LAUNCH_ABSENT"))
      return NULL;
    const char *v = getenv("TASM2_GAIA_FIRST_LAUNCH");
    return (v && *v) ? v : "false";
  }
  return NULL;
}

static const char *shared_value_get(const char *key) {
  int i = shared_value_find(key);
  if (i >= 0)
    return g_shared_values[i].value;
  const char *fallback = shared_value_default(key);
  return fallback ? fallback : "";
}

static int shared_value_has(const char *key) {
  return shared_value_find(key) >= 0 || shared_value_default(key) != NULL;
}

static void shared_value_set(const char *key, const char *value) {
  if (!key || !*key)
    return;
  int i = shared_value_find(key);
  if (i < 0) {
    for (int k = 0; k < MAX_SHARED_VALUES; k++) {
      if (!g_shared_values[k].used) {
        i = k;
        break;
      }
    }
  }
  if (i < 0)
    i = 0;
  g_shared_values[i].used = 1;
  copy_text(g_shared_values[i].key, sizeof(g_shared_values[i].key), key);
  copy_text(g_shared_values[i].value, sizeof(g_shared_values[i].value),
            value ? value : "");
}

static void shared_value_delete(const char *key) {
  int i = shared_value_find(key);
  if (i >= 0)
    g_shared_values[i].used = 0;
}

/* ---- Generic stub ---- */
static intptr_t jni_stub(void) { return 0; }

/* ---- JNIEnv functions ---- */

static jint jni_GetVersion(void *env) {
  (void)env;
  return 0x00010006;
}

static void *jni_FindClass(void *env, const char *name) {
  (void)env;
  debugPrintf("jni_shim: FindClass(%s)\n", name);
  jni_trace("jni_shim: FindClass(%s)\n", name);
  return remember_fake_class(name);
}

static void *jni_GetMethodID(void *env, void *clazz, const char *name,
                             const char *sig) {
  (void)env;
  debugPrintf("jni_shim: GetMethodID(%s, %s)\n", name, sig);
#define RETURN_METHOD_TAG(tag) return remember_known_id((tag), "method", name, sig)
  const char *class_name = resolve_fake_class(clazz);
  int is_bundle = strcmp(class_name, "android/os/Bundle") == 0;
  if (is_bundle && strcmp(name, "<init>") == 0)
    RETURN_METHOD_TAG(MID_BUNDLE_CTOR);
  if (is_bundle && strcmp(name, "clear") == 0)
    RETURN_METHOD_TAG(MID_BUNDLE_CLEAR);
  if (is_bundle && strcmp(name, "containsKey") == 0)
    RETURN_METHOD_TAG(MID_BUNDLE_CONTAINS_KEY);
  if (is_bundle && strcmp(name, "getString") == 0)
    RETURN_METHOD_TAG(MID_BUNDLE_GET_STRING);
  if (is_bundle && strcmp(name, "getInt") == 0)
    RETURN_METHOD_TAG(MID_BUNDLE_GET_INT);
  if (is_bundle && strcmp(name, "getLong") == 0)
    RETURN_METHOD_TAG(MID_BUNDLE_GET_LONG);
  if (is_bundle && strcmp(name, "getBoolean") == 0)
    RETURN_METHOD_TAG(MID_BUNDLE_GET_BOOLEAN);
  if (is_bundle && strcmp(name, "getByteArray") == 0)
    RETURN_METHOD_TAG(MID_BUNDLE_GET_BYTE_ARRAY);
  if (is_bundle && strcmp(name, "putString") == 0)
    RETURN_METHOD_TAG(MID_BUNDLE_PUT_STRING);
  if (is_bundle && strcmp(name, "putInt") == 0)
    RETURN_METHOD_TAG(MID_BUNDLE_PUT_INT);
  if (is_bundle && strcmp(name, "putLong") == 0)
    RETURN_METHOD_TAG(MID_BUNDLE_PUT_LONG);
  if (is_bundle && strcmp(name, "putBoolean") == 0)
    RETURN_METHOD_TAG(MID_BUNDLE_PUT_BOOLEAN);
  if (is_bundle && strcmp(name, "putByteArray") == 0)
    RETURN_METHOD_TAG(MID_BUNDLE_PUT_BYTE_ARRAY);
  if (is_bundle && strcmp(name, "putAll") == 0)
    RETURN_METHOD_TAG(MID_BUNDLE_PUT_ALL);
  if (strcmp(name, "getClassLoader") == 0)
    RETURN_METHOD_TAG(MID_GET_CLASS_LOADER);
  if (strcmp(name, "loadClass") == 0)
    RETURN_METHOD_TAG(MID_LOAD_CLASS);
  if (strcmp(name, "GetDefaultAudioStreamParameters") == 0)
    RETURN_METHOD_TAG(MID_AUDIO_STREAM_PARAMS);
  /* 🔋 bateria: a engine 10tons reduz pra 30fps em modo economia. Respondemos
   * 100% + carregando (level garbage/0 = power-save = cap de fps!). */
  if (strcmp(name, "getBatteryLevel") == 0)
    RETURN_METHOD_TAG(MID_BATTERY_LEVEL);
  if (strcmp(name, "getBatteryStatus") == 0)
    RETURN_METHOD_TAG(MID_BATTERY_STATUS);
  /* 🇬🇧 idioma: SEMPRE inglês (regra do projeto). Locale.getLanguage()->"en". */
  if (strcmp(name, "getLanguage") == 0)
    RETURN_METHOD_TAG(MID_GET_LANGUAGE);
  if (strcmp(name, "getCountry") == 0)
    RETURN_METHOD_TAG(MID_GET_COUNTRY);
  return remember_dynamic_id("method", name, sig);
#undef RETURN_METHOD_TAG
}

static void *jni_GetStaticMethodID(void *env, void *clazz, const char *name,
                                   const char *sig) {
  (void)env;
  debugPrintf("jni_shim: GetStaticMethodID(%s, %s)\n", name, sig);
#define RETURN_STATIC_TAG(tag) return remember_known_id((tag), "static-method", name, sig)
  const char *class_name = resolve_fake_class(clazz);
  if (strcmp(name, "getData") == 0 && strstr(sig, "Landroid/os/Bundle;") &&
      strstr(class_name, "InAppBilling"))
    RETURN_STATIC_TAG(MID_IAP_GET_DATA);
  if (strcmp(name, "getStorageDir") == 0)
    RETURN_STATIC_TAG(MID_GET_STORAGE_DIR);
  if (strcmp(name, "getPackName") == 0)
    RETURN_STATIC_TAG(MID_GET_PACK_NAME);
  if (strcmp(name, "getPreferenceString") == 0)
    RETURN_STATIC_TAG(MID_GET_PREFERENCE_STRING);
  if (strcmp(name, "getPackage") == 0)
    RETURN_STATIC_TAG(MID_GET_PACKAGE);
  if (strcmp(name, "getSaveFolder") == 0)
    RETURN_STATIC_TAG(MID_GET_SAVE_FOLDER);
  if (strcmp(name, "getContext") == 0)
    RETURN_STATIC_TAG(MID_GET_CONTEXT);
  if (strcmp(name, "getUserAgent") == 0)
    RETURN_STATIC_TAG(MID_GET_USER_AGENT);
  if (strcmp(name, "getResource") == 0)
    RETURN_STATIC_TAG(MID_GET_RESOURCE);
  if (strcmp(name, "setupPaths") == 0)
    RETURN_STATIC_TAG(MID_SETUP_PATHS);
  if (strcmp(name, "createView") == 0)
    RETURN_STATIC_TAG(MID_CREATE_VIEW);
  if (strcmp(name, "setViewSettings") == 0)
    RETURN_STATIC_TAG(MID_SET_VIEW_SETTINGS);
  if (strcmp(name, "setCurrentContext") == 0)
    RETURN_STATIC_TAG(MID_SET_CURRENT_CONTEXT);
  if (strcmp(name, "CheckHDMIState") == 0)
    RETURN_STATIC_TAG(MID_CHECK_HDMI_STATE);
  if (strcmp(name, "GetHDMIName") == 0)
    RETURN_STATIC_TAG(MID_GET_HDMI_NAME);
  if (strcmp(name, "getUDID") == 0)
    RETURN_STATIC_TAG(MID_GET_UDID);
  if (strcmp(name, "getHDIDFV") == 0)
    RETURN_STATIC_TAG(MID_GET_HIDFV);
  if (strcmp(name, "getCountry") == 0)
    RETURN_STATIC_TAG(MID_GET_COUNTRY);
  if (strcmp(name, "getAndroidId") == 0)
    RETURN_STATIC_TAG(MID_GET_ANDROID_ID);
  if (strcmp(name, "GetWindowWidth") == 0)
    RETURN_STATIC_TAG(MID_GET_WINDOW_WIDTH);
  if (strcmp(name, "GetWindowHeight") == 0)
    RETURN_STATIC_TAG(MID_GET_WINDOW_HEIGHT);
  if (strcmp(name, "getResolutionX") == 0)
    RETURN_STATIC_TAG(MID_GET_RESOLUTION_X);
  if (strcmp(name, "getResolutionY") == 0)
    RETURN_STATIC_TAG(MID_GET_RESOLUTION_Y);
  if (strcmp(name, "sLaunchVideoPlayer") == 0)
    RETURN_STATIC_TAG(MID_LAUNCH_VIDEO_PLAYER);
  if (strcmp(name, "sGetKeyboardText") == 0)
    RETURN_STATIC_TAG(MID_GET_KEYBOARD_TEXT);
  if (strcmp(name, "sIsKeyboardVisible") == 0)
    RETURN_STATIC_TAG(MID_IS_KEYBOARD_VISIBLE);
  if (strcmp(name, "GetManufacturer") == 0)
    RETURN_STATIC_TAG(MID_GET_MANUFACTURER);
  if (strcmp(name, "GetDeviceName") == 0)
    RETURN_STATIC_TAG(MID_GET_DEVICE_NAME);
  if (strcmp(name, "GetDeviceFirmware") == 0)
    RETURN_STATIC_TAG(MID_GET_DEVICE_FIRMWARE);
  if (strcmp(name, "JGetMaxCPUSpeed") == 0)
    RETURN_STATIC_TAG(MID_GET_CPU_MAX);
  if (strcmp(name, "JGetCurrentCPUSpeed") == 0)
    RETURN_STATIC_TAG(MID_GET_CPU_CURRENT);
  if (strcmp(name, "JGetMaxAvailableRam") == 0)
    RETURN_STATIC_TAG(MID_GET_MAX_RAM);
  if (strcmp(name, "JGetFreeDiskSpace") == 0)
    RETURN_STATIC_TAG(MID_GET_FREE_DISK);
  if (strcmp(name, "JGetFreeRam") == 0)
    RETURN_STATIC_TAG(MID_GET_FREE_RAM);
  if (strcmp(name, "GetDeviceLanguage") == 0)
    RETURN_STATIC_TAG(MID_GET_DEVICE_LANGUAGE);
  if (strcmp(name, "GetDeviceWidth") == 0)
    RETURN_STATIC_TAG(MID_GET_DEVICE_WIDTH);
  if (strcmp(name, "GetDeviceHeight") == 0)
    RETURN_STATIC_TAG(MID_GET_DEVICE_HEIGHT);
  if (strcmp(name, "GetUptimeSystem") == 0)
    RETURN_STATIC_TAG(MID_GET_UPTIME);
  if (strcmp(name, "getGameName") == 0)
    RETURN_STATIC_TAG(MID_GET_GAME_NAME);
  if (strcmp(name, "getInjectedIGP") == 0)
    RETURN_STATIC_TAG(MID_GET_INJECTED_IGP);
  if (strcmp(name, "getInjectedSerialKey") == 0)
    RETURN_STATIC_TAG(MID_GET_INJECTED_SERIAL_KEY);
  if (strcmp(name, "getSDFolder") == 0)
    RETURN_STATIC_TAG(MID_GET_SD_FOLDER);
  if (strcmp(name, "GetApkPath") == 0)
    RETURN_STATIC_TAG(MID_GET_APK_PATH);
  if (strcmp(name, "getAssetAsString") == 0)
    RETURN_STATIC_TAG(MID_GET_ASSET_AS_STRING);
  if (strcmp(name, "getMetaDataValue") == 0)
    RETURN_STATIC_TAG(MID_GET_META_DATA_VALUE);
  if (strcmp(name, "nativeSetPreference") == 0 &&
      strstr(sig, "Landroid/os/Bundle;"))
    RETURN_STATIC_TAG(MID_NATIVE_SET_PREFERENCE);
  if (strcmp(name, "nativeGetPreference") == 0 &&
      strstr(sig, "Landroid/os/Bundle;"))
    RETURN_STATIC_TAG(MID_NATIVE_GET_PREFERENCE);
  if (strcmp(name, "initCheckConnectionType") == 0)
    RETURN_STATIC_TAG(MID_INIT_CHECK_CONNECTION);
  if (strcmp(name, "retrieveBarrels") == 0)
    RETURN_STATIC_TAG(MID_RETRIEVE_BARRELS);
  if (strcmp(name, "getGLUID") == 0)
    RETURN_STATIC_TAG(MID_GET_GLUID);
  if (strcmp(name, "d1") == 0)
    RETURN_STATIC_TAG(MID_D1);
  if (strcmp(name, "getSerial") == 0)
    RETURN_STATIC_TAG(MID_GET_SERIAL);
  if (strcmp(name, "getSerialNo") == 0)
    RETURN_STATIC_TAG(MID_GET_SERIAL_NO);
  if (strcmp(name, "getDeviceFirmware") == 0)
    RETURN_STATIC_TAG(MID_GET_DEVICE_FIRMWARE_JAVA);
  if (strcmp(name, "getMacAddress") == 0)
    RETURN_STATIC_TAG(MID_GET_MAC_ADDRESS);
  if (strcmp(name, "getDeviceIMEI") == 0)
    RETURN_STATIC_TAG(MID_GET_DEVICE_IMEI);
  if (strcmp(name, "getHDIDFVVersion") == 0)
    RETURN_STATIC_TAG(MID_GET_HIDFV_VERSION);
  if (strcmp(name, "getGoogleAdId") == 0)
    RETURN_STATIC_TAG(MID_GET_GOOGLE_AD_ID);
  if (strcmp(name, "getGoogleAdIdStatus") == 0)
    RETURN_STATIC_TAG(MID_GET_GOOGLE_AD_ID_STATUS);
  if (strcmp(name, "getGLDID") == 0)
    RETURN_STATIC_TAG(MID_GET_GLDID);
  if (strcmp(name, "getDeviceName") == 0)
    RETURN_STATIC_TAG(MID_GET_DEVICE_NAME_JAVA);
  if (strcmp(name, "getPhoneManufacturer") == 0)
    RETURN_STATIC_TAG(MID_GET_PHONE_MANUFACTURER);
  if (strcmp(name, "getPhoneModel") == 0)
    RETURN_STATIC_TAG(MID_GET_PHONE_MODEL);
  if (strcmp(name, "retrieveDeviceCarrier") == 0)
    RETURN_STATIC_TAG(MID_RETRIEVE_DEVICE_CARRIER);
  if (strcmp(name, "retrieveDeviceCountry") == 0)
    RETURN_STATIC_TAG(MID_RETRIEVE_DEVICE_COUNTRY);
  if (strcmp(name, "retrieveDeviceRegion") == 0)
    RETURN_STATIC_TAG(MID_RETRIEVE_DEVICE_REGION);
  if (strcmp(name, "retrieveDeviceLanguage") == 0)
    RETURN_STATIC_TAG(MID_RETRIEVE_DEVICE_LANGUAGE);
  if (strcmp(name, "retrieveCPUSerial") == 0)
    RETURN_STATIC_TAG(MID_RETRIEVE_CPU_SERIAL);
  if (strcmp(name, "getPhoneDevice") == 0)
    RETURN_STATIC_TAG(MID_GET_PHONE_DEVICE);
  if (strcmp(name, "getPhoneProduct") == 0)
    RETURN_STATIC_TAG(MID_GET_PHONE_PRODUCT);
  if (strcmp(name, "setSharedValue") == 0)
    RETURN_STATIC_TAG(MID_SET_SHARED_VALUE);
  if (strcmp(name, "getSharedValue") == 0)
    RETURN_STATIC_TAG(MID_GET_SHARED_VALUE);
  if (strcmp(name, "deleteSharedValue") == 0)
    RETURN_STATIC_TAG(MID_DELETE_SHARED_VALUE);
  if (strcmp(name, "isSharedValue") == 0)
    RETURN_STATIC_TAG(MID_IS_SHARED_VALUE);
  if (strcmp(name, "sGetMilliseconds") == 0)
    RETURN_STATIC_TAG(MID_GET_MILLISECONDS);
  if (strcmp(name, "setActivity") == 0)
    RETURN_STATIC_TAG(MID_SET_ACTIVITY);
  if (strcmp(name, "errorDialog") == 0)
    RETURN_STATIC_TAG(MID_ERROR_DIALOG);
  /* Locale.getDefault() -> objeto Locale (não-nulo) p/ chamar getLanguage() */
  if (strcmp(name, "getDefault") == 0)
    RETURN_STATIC_TAG(MID_GET_DEFAULT_LOCALE);
  return remember_dynamic_id("static-method", name, sig);
#undef RETURN_STATIC_TAG
}

static void *jni_GetFieldID(void *env, void *clazz, const char *name,
                            const char *sig) {
  (void)env;
  (void)clazz;
  debugPrintf("jni_shim: GetFieldID(%s, %s)\n", name, sig);
  return remember_dynamic_id("field", name, sig);
}

static void *jni_GetStaticFieldID(void *env, void *clazz, const char *name,
                                  const char *sig) {
  (void)env;
  (void)clazz;
  debugPrintf("jni_shim: GetStaticFieldID(%s, %s)\n", name, sig);
  if (strcmp(name, "OBB_VERSIONCODE") == 0)
    return remember_known_id(FID_OBB_VERSIONCODE, "static-field", name, sig);
  if (strcmp(name, "MANUFACTURER") == 0)
    return remember_known_id(FID_BUILD_MANUFACTURER, "static-field", name, sig);
  if (strcmp(name, "MODEL") == 0)
    return remember_known_id(FID_BUILD_MODEL, "static-field", name, sig);
  if (strcmp(name, "DEVICE") == 0)
    return remember_known_id(FID_BUILD_DEVICE, "static-field", name, sig);
  if (strcmp(name, "PRODUCT") == 0)
    return remember_known_id(FID_BUILD_PRODUCT, "static-field", name, sig);
  if (strcmp(name, "RELEASE") == 0)
    return remember_known_id(FID_BUILD_VERSION_RELEASE, "static-field", name, sig);
  return remember_dynamic_id("static-field", name, sig);
}

/* Array fake de parâmetros de áudio: [sampleRate, framesPerBurst]
 * (Oboe/engine pedem via GetDefaultAudioStreamParameters()[I]) */
static jint g_audio_params[2] = {44100, 1024};
static void *alloc_fake_array(const void *data, int len, int elem_size,
                              int copy_data);

/* ---- Fake Java objects / android.os.Bundle ---- */
enum {
  OBJ_GENERIC = 0,
  OBJ_BUNDLE,
  OBJ_CONTEXT,
  OBJ_CLASS_LOADER,
};

enum {
  BVAL_NONE = 0,
  BVAL_STRING,
  BVAL_INT,
  BVAL_LONG,
  BVAL_BOOLEAN,
  BVAL_OBJECT,
};

#define MAX_FAKE_OBJECTS 64
#define MAX_BUNDLE_ENTRIES 64

struct fake_bundle_entry {
  int used;
  int type;
  char key[128];
  char s[256];
  jint i;
  long long j;
  unsigned char z;
  void *obj;
};

struct fake_object {
  void *handle;
  int kind;
  struct fake_bundle_entry entries[MAX_BUNDLE_ENTRIES];
};

static struct fake_object g_fake_objects[MAX_FAKE_OBJECTS];
static char g_fake_object_handles[MAX_FAKE_OBJECTS];
static int g_fake_object_next;

static void *alloc_fake_object(int kind) {
  int idx = g_fake_object_next++ % MAX_FAKE_OBJECTS;
  memset(&g_fake_objects[idx], 0, sizeof(g_fake_objects[idx]));
  g_fake_objects[idx].handle = &g_fake_object_handles[idx];
  g_fake_objects[idx].kind = kind;
  jni_trace("jni_shim: fake object kind=%d -> %p\n", kind,
            g_fake_objects[idx].handle);
  return g_fake_objects[idx].handle;
}

static int fake_object_index(void *obj) {
  if (!obj)
    return -1;
  for (int i = 0; i < MAX_FAKE_OBJECTS; i++) {
    if (g_fake_objects[i].handle == obj)
      return i;
  }
  return -1;
}

static int bundle_index(void *obj) {
  int idx = fake_object_index(obj);
  if (idx >= 0 && g_fake_objects[idx].kind == OBJ_BUNDLE)
    return idx;
  return -1;
}

static struct fake_bundle_entry *bundle_find_entry(int bundle_idx,
                                                   const char *key) {
  if (bundle_idx < 0 || !key)
    return NULL;
  for (int i = 0; i < MAX_BUNDLE_ENTRIES; i++) {
    struct fake_bundle_entry *e = &g_fake_objects[bundle_idx].entries[i];
    if (e->used && strcmp(e->key, key) == 0)
      return e;
  }
  return NULL;
}

static struct fake_bundle_entry *bundle_put_entry(void *bundle,
                                                  const char *key,
                                                  int type) {
  int bundle_idx = bundle_index(bundle);
  if (bundle_idx < 0 || !key || !*key)
    return NULL;
  struct fake_bundle_entry *e = bundle_find_entry(bundle_idx, key);
  if (!e) {
    for (int i = 0; i < MAX_BUNDLE_ENTRIES; i++) {
      if (!g_fake_objects[bundle_idx].entries[i].used) {
        e = &g_fake_objects[bundle_idx].entries[i];
        break;
      }
    }
  }
  if (!e)
    e = &g_fake_objects[bundle_idx].entries[0];
  memset(e, 0, sizeof(*e));
  e->used = 1;
  e->type = type;
  copy_text(e->key, sizeof(e->key), key);
  return e;
}

static int bundle_key_default_int(const char *key, jint *out) {
  if (!key || !*key || !out)
    return 0;
  if (strlen(key) == 1 && strchr("RSECO", key[0])) {
    *out = (key[0] == 'R' || key[0] == 'S') ? 1 : 0;
    return 1;
  }
  if (strstr(key, "RESPONSE") || strstr(key, "response") ||
      strstr(key, "RESULT") || strstr(key, "result") ||
      strstr(key, "ERROR") || strstr(key, "error") ||
      strcmp(key, "status") == 0 || strcmp(key, "Status") == 0) {
    *out = 0;
    return 1;
  }
  if (strstr(key, "demo") || strstr(key, "DEMO")) {
    *out = 0;
    return 1;
  }
  if (strstr(key, "purchase") || strstr(key, "Purchase") ||
      strstr(key, "owned") || strstr(key, "Owned") ||
      strstr(key, "full") || strstr(key, "Full")) {
    *out = 1;
    return 1;
  }
  return 0;
}

static int bundle_key_default_bool(const char *key, unsigned char *out) {
  jint i = 0;
  if (!bundle_key_default_int(key, &i))
    return 0;
  *out = i ? 1 : 0;
  return 1;
}

static const char *bundle_key_default_string(const char *key) {
  if (!key || !*key)
    return NULL;
  if (strlen(key) == 1 && strchr("PITD", key[0]))
    return key[0] == 'D' ? "" : "AndroidFullGame";
  if (strstr(key, "Product") || strstr(key, "product") ||
      strstr(key, "item") || strstr(key, "Item"))
    return "AndroidFullGame";
  if (strstr(key, "price") || strstr(key, "Price"))
    return "0.00";
  if (strstr(key, "currency") || strstr(key, "Currency"))
    return "USD";
  if (strstr(key, "receipt") || strstr(key, "Receipt") ||
      strstr(key, "token") || strstr(key, "Token") ||
      strstr(key, "order") || strstr(key, "Order") ||
      strstr(key, "signature") || strstr(key, "Signature") ||
      strstr(key, "purchase") || strstr(key, "Purchase"))
    return "nextos-tasm2-purchase";
  if (strstr(key, "title") || strstr(key, "Title") ||
      strstr(key, "name") || strstr(key, "Name"))
    return "The Amazing Spider-Man 2";
  return NULL;
}

static int bundle_key_has_default(const char *key) {
  jint i = 0;
  unsigned char z = 0;
  return bundle_key_default_int(key, &i) ||
         bundle_key_default_bool(key, &z) ||
         bundle_key_default_string(key) != NULL;
}

static void bundle_clear(void *bundle) {
  int idx = bundle_index(bundle);
  if (idx < 0)
    return;
  memset(g_fake_objects[idx].entries, 0,
         sizeof(g_fake_objects[idx].entries));
  jni_trace("jni_shim:   -> Bundle.clear(%p)\n", bundle);
}

static void bundle_copy(void *dst, void *src) {
  int di = bundle_index(dst);
  int si = bundle_index(src);
  if (di < 0 || si < 0)
    return;
  memcpy(g_fake_objects[di].entries, g_fake_objects[si].entries,
         sizeof(g_fake_objects[di].entries));
}

static void bundle_put_string(void *bundle, const char *key,
                              const char *value) {
  struct fake_bundle_entry *e = bundle_put_entry(bundle, key, BVAL_STRING);
  if (!e)
    return;
  copy_text(e->s, sizeof(e->s), value ? value : "");
  jni_trace("jni_shim:   -> Bundle.putString(\"%s\", \"%s\")\n", key,
            e->s);
}

static void bundle_put_int(void *bundle, const char *key, jint value) {
  struct fake_bundle_entry *e = bundle_put_entry(bundle, key, BVAL_INT);
  if (!e)
    return;
  e->i = value;
  jni_trace("jni_shim:   -> Bundle.putInt(\"%s\", %d)\n", key,
            (int)value);
}

static void bundle_put_long(void *bundle, const char *key, long long value) {
  struct fake_bundle_entry *e = bundle_put_entry(bundle, key, BVAL_LONG);
  if (!e)
    return;
  e->j = value;
  jni_trace("jni_shim:   -> Bundle.putLong(\"%s\", %lld)\n", key, value);
}

static void bundle_put_boolean(void *bundle, const char *key,
                               unsigned char value) {
  struct fake_bundle_entry *e = bundle_put_entry(bundle, key, BVAL_BOOLEAN);
  if (!e)
    return;
  e->z = value ? 1 : 0;
  jni_trace("jni_shim:   -> Bundle.putBoolean(\"%s\", %d)\n", key,
            e->z ? 1 : 0);
}

static void bundle_put_object(void *bundle, const char *key, void *value) {
  struct fake_bundle_entry *e = bundle_put_entry(bundle, key, BVAL_OBJECT);
  if (!e)
    return;
  e->obj = value;
  jni_trace("jni_shim:   -> Bundle.putObject(\"%s\", %p)\n", key, value);
}

static void bundle_put_byte_array(void *bundle, const char *key,
                                  const void *data, int len) {
  void *array = alloc_fake_array(data, len, 1, 1);
  struct fake_bundle_entry *e = bundle_put_entry(bundle, key, BVAL_OBJECT);
  if (!e)
    return;
  e->obj = array;
  jni_trace("jni_shim:   -> Bundle.putByteArray(\"%s\", %d bytes) = %p\n",
            key, len, array);
}

static void bundle_dump(void *bundle, const char *label) {
  if (!jni_trace_enabled())
    return;
  int idx = bundle_index(bundle);
  jni_trace("jni_shim:   -> Bundle.dump %s %p idx=%d\n",
            label ? label : "", bundle, idx);
  if (idx < 0)
    return;
  for (int i = 0; i < MAX_BUNDLE_ENTRIES; i++) {
    struct fake_bundle_entry *e = &g_fake_objects[idx].entries[i];
    if (!e->used)
      continue;
    switch (e->type) {
    case BVAL_STRING:
      jni_trace("jni_shim:      %s = string \"%s\"\n", e->key, e->s);
      break;
    case BVAL_INT:
      jni_trace("jni_shim:      %s = int %d\n", e->key, (int)e->i);
      break;
    case BVAL_LONG:
      jni_trace("jni_shim:      %s = long %lld\n", e->key, e->j);
      break;
    case BVAL_BOOLEAN:
      jni_trace("jni_shim:      %s = bool %d\n", e->key, e->z ? 1 : 0);
      break;
    case BVAL_OBJECT:
      jni_trace("jni_shim:      %s = object %p\n", e->key, e->obj);
      break;
    default:
      jni_trace("jni_shim:      %s = type %d\n", e->key, e->type);
      break;
    }
  }
}

static int bundle_has_key(void *bundle, const char *key) {
  int idx = bundle_index(bundle);
  int has = idx >= 0 && bundle_find_entry(idx, key) != NULL;
  if (!has)
    has = bundle_key_has_default(key);
  jni_trace("jni_shim:   -> Bundle.containsKey(\"%s\") = %d\n",
            key ? key : "", has ? 1 : 0);
  return has;
}

static const char *bundle_get_string(void *bundle, const char *key,
                                     const char *default_value) {
  int idx = bundle_index(bundle);
  struct fake_bundle_entry *e = bundle_find_entry(idx, key);
  if (e) {
    if (e->type == BVAL_STRING)
      return e->s;
    if (e->type == BVAL_INT) {
      snprintf(e->s, sizeof(e->s), "%d", (int)e->i);
      return e->s;
    }
    if (e->type == BVAL_LONG) {
      snprintf(e->s, sizeof(e->s), "%lld", e->j);
      return e->s;
    }
    if (e->type == BVAL_BOOLEAN)
      return e->z ? "1" : "0";
  }
  const char *fallback = bundle_key_default_string(key);
  if (fallback)
    return fallback;
  return default_value ? default_value : "";
}

static jint bundle_get_int(void *bundle, const char *key,
                           jint default_value) {
  int idx = bundle_index(bundle);
  struct fake_bundle_entry *e = bundle_find_entry(idx, key);
  if (e) {
    if (e->type == BVAL_INT)
      return e->i;
    if (e->type == BVAL_LONG)
      return (jint)e->j;
    if (e->type == BVAL_BOOLEAN)
      return e->z ? 1 : 0;
    if (e->type == BVAL_STRING)
      return (jint)strtol(e->s, NULL, 10);
  }
  jint fallback = 0;
  if (bundle_key_default_int(key, &fallback))
    return fallback;
  return default_value;
}

static long long bundle_get_long(void *bundle, const char *key,
                                 long long default_value) {
  int idx = bundle_index(bundle);
  struct fake_bundle_entry *e = bundle_find_entry(idx, key);
  if (e) {
    if (e->type == BVAL_LONG)
      return e->j;
    if (e->type == BVAL_INT)
      return e->i;
    if (e->type == BVAL_BOOLEAN)
      return e->z ? 1 : 0;
    if (e->type == BVAL_STRING)
      return strtoll(e->s, NULL, 10);
  }
  jint fallback = 0;
  if (bundle_key_default_int(key, &fallback))
    return fallback;
  return default_value;
}

static unsigned char bundle_get_boolean(void *bundle, const char *key,
                                        unsigned char default_value) {
  int idx = bundle_index(bundle);
  struct fake_bundle_entry *e = bundle_find_entry(idx, key);
  if (e) {
    if (e->type == BVAL_BOOLEAN)
      return e->z ? 1 : 0;
    if (e->type == BVAL_INT)
      return e->i ? 1 : 0;
    if (e->type == BVAL_LONG)
      return e->j ? 1 : 0;
    if (e->type == BVAL_STRING)
      return (strcmp(e->s, "true") == 0 || strcmp(e->s, "1") == 0) ? 1 : 0;
  }
  unsigned char fallback = 0;
  if (bundle_key_default_bool(key, &fallback))
    return fallback;
  return default_value;
}

static void *bundle_get_object(void *bundle, const char *key) {
  int idx = bundle_index(bundle);
  struct fake_bundle_entry *e = bundle_find_entry(idx, key);
  return e && e->type == BVAL_OBJECT ? e->obj : NULL;
}

static void bundle_seed_iap_response(void *bundle, int op) {
  static const char iap_response[] =
      "{\"status\":0,\"success\":true,\"purchased\":true,"
      "\"productId\":\"AndroidFullGame\","
      "\"receipt\":\"nextos-tasm2-receipt\","
      "\"purchaseToken\":\"nextos-tasm2-token\","
      "\"signature\":\"nextos-tasm2-signature\"}";
  bundle_put_int(bundle, "RESPONSE_CODE", 0);
  bundle_put_int(bundle, "RESPONSE_CODE_VALUE", 0);
  bundle_put_int(bundle, "RESULT_CODE", 0);
  bundle_put_int(bundle, "ERROR_CODE", 0);
  bundle_put_int(bundle, "error_code", 0);
  bundle_put_int(bundle, "status", 0);
  bundle_put_int(bundle, "E", 0);
  bundle_put_int(bundle, "C", 0);
  bundle_put_int(bundle, "is_demo_version", 0);
  if (op == 8 && !getenv("TASM2_IAP_O8_PAYLOAD")) {
    jni_trace("jni_shim:   -> IAP O=8 poll vazio, sem payload R\n");
    return;
  }
  bundle_put_int(bundle, "R", 0);
  bundle_put_boolean(bundle, "success", 1);
  bundle_put_boolean(bundle, "S", 1);
  bundle_put_boolean(bundle, "purchased", 1);
  bundle_put_boolean(bundle, "is_purchased", 1);
  bundle_put_boolean(bundle, "full_game", 1);
  bundle_put_string(bundle, "ProductId", "AndroidFullGame");
  bundle_put_string(bundle, "productId", "AndroidFullGame");
  bundle_put_string(bundle, "product_id", "AndroidFullGame");
  bundle_put_string(bundle, "Iap_item", "AndroidFullGame");
  bundle_put_string(bundle, "Iap_hp", "AndroidFullGame");
  bundle_put_string(bundle, "Buy_full_game", "AndroidFullGame");
  bundle_put_string(bundle, "P", "AndroidFullGame");
  bundle_put_string(bundle, "I", "AndroidFullGame");
  bundle_put_string(bundle, "T", "AndroidFullGame");
  bundle_put_string(bundle, "D", "");
  bundle_put_string(bundle, "price", "0.00");
  bundle_put_string(bundle, "currency", "USD");
  bundle_put_string(bundle, "receipt", "nextos-tasm2-receipt");
  bundle_put_string(bundle, "purchaseToken", "nextos-tasm2-token");
  bundle_put_string(bundle, "orderId", "nextos-tasm2-order");
  bundle_put_string(bundle, "signature", "nextos-tasm2-signature");
  bundle_put_byte_array(bundle, "R", iap_response,
                        (int)sizeof(iap_response) - 1);
}

static int object_method_arg_count(void *methodID) {
  const char *name = id_name(methodID);
  if (methodID == &g_method_tags[MID_BUNDLE_CLEAR])
    return 0;
  if (methodID == &g_method_tags[MID_BUNDLE_CONTAINS_KEY] ||
      methodID == &g_method_tags[MID_BUNDLE_GET_STRING] ||
      methodID == &g_method_tags[MID_BUNDLE_GET_INT] ||
      methodID == &g_method_tags[MID_BUNDLE_GET_LONG] ||
      methodID == &g_method_tags[MID_BUNDLE_GET_BOOLEAN] ||
      methodID == &g_method_tags[MID_BUNDLE_GET_BYTE_ARRAY])
    return 1;
  if (methodID == &g_method_tags[MID_BUNDLE_PUT_STRING] ||
      methodID == &g_method_tags[MID_BUNDLE_PUT_INT] ||
      methodID == &g_method_tags[MID_BUNDLE_PUT_LONG] ||
      methodID == &g_method_tags[MID_BUNDLE_PUT_BOOLEAN] ||
      methodID == &g_method_tags[MID_BUNDLE_PUT_BYTE_ARRAY])
    return 2;
  if (methodID == &g_method_tags[MID_BUNDLE_PUT_ALL])
    return 1;
  if (strcmp(name, "getString") == 0 || strcmp(name, "getByteArray") == 0 ||
      strcmp(name, "containsKey") == 0)
    return 1;
  if (strncmp(name, "put", 3) == 0)
    return 2;
  return 0;
}

enum {
  ARG_OBJECT = 0,
  ARG_INT,
  ARG_LONG,
};

static int object_method_arg_type(void *methodID, int idx) {
  if (idx == 1 &&
      (methodID == &g_method_tags[MID_BUNDLE_PUT_INT] ||
       methodID == &g_method_tags[MID_BUNDLE_PUT_BOOLEAN]))
    return ARG_INT;
  if (idx == 1 && methodID == &g_method_tags[MID_BUNDLE_PUT_LONG])
    return ARG_LONG;
  return ARG_OBJECT;
}

static void set_object_args_from_va(void *methodID, va_list args) {
  int argc = object_method_arg_count(methodID);
  va_list ap;
  va_copy(ap, args);
  g_static_args_override = 1;
  for (int i = 0; i < argc && i < 4; i++) {
    int type = object_method_arg_type(methodID, i);
    if (type == ARG_INT) {
      g_static_args[i] = (void *)(intptr_t)va_arg(ap, int);
    } else if (type == ARG_LONG) {
      g_static_long_args[i] = va_arg(ap, long long);
      g_static_args[i] = (void *)(intptr_t)g_static_long_args[i];
    } else {
      g_static_args[i] = va_arg(ap, void *);
    }
  }
  va_end(ap);
}

static void set_object_args_from_jvalue(void *methodID, const jvalue *args) {
  int argc = object_method_arg_count(methodID);
  g_static_args_override = 1;
  for (int i = 0; i < argc && i < 4; i++) {
    int type = object_method_arg_type(methodID, i);
    if (type == ARG_INT) {
      g_static_args[i] = (void *)(intptr_t)(args ? args[i].i : 0);
    } else if (type == ARG_LONG) {
      g_static_long_args[i] = args ? args[i].j : 0;
      g_static_args[i] = (void *)(intptr_t)g_static_long_args[i];
    } else {
      g_static_args[i] = args ? args[i].l : NULL;
    }
  }
}

/* CallObjectMethod (index 36) - variadic */
static void *jni_CallObjectMethod(void *env, void *obj, void *methodID, ...) {
  (void)env;
  log_call_id("CallObjectMethod", methodID);
  if (methodID == &g_method_tags[MID_AUDIO_STREAM_PARAMS]) {
    debugPrintf("jni_shim:   -> AudioStreamParameters jintArray {%d,%d}\n",
                g_audio_params[0], g_audio_params[1]);
    return g_audio_params;
  }
  /* 🇬🇧 Locale.getLanguage()/getCountry() -> inglês (regra do projeto) */
  if (methodID == &g_method_tags[MID_GET_LANGUAGE]) {
    debugPrintf("jni_shim:   -> getLanguage = \"en\"\n");
    return make_jstring("en");
  }
  if (methodID == &g_method_tags[MID_GET_COUNTRY]) {
    debugPrintf("jni_shim:   -> getCountry = \"US\"\n");
    return make_jstring("US");
  }
  if (methodID == &g_method_tags[MID_BUNDLE_GET_STRING]) {
    void *key_str = NULL;
    void *default_str = NULL;
    if (g_static_args_override) {
      key_str = g_static_args[0];
      default_str = g_static_args[1];
    } else {
      va_list ap;
      va_start(ap, methodID);
      key_str = va_arg(ap, void *);
      if (strstr(id_sig(methodID), "Ljava/lang/String;Ljava/lang/String;"))
        default_str = va_arg(ap, void *);
      va_end(ap);
    }
    const char *key = resolve_jstring(key_str);
    const char *value = bundle_get_string(obj, key, resolve_jstring(default_str));
    jni_trace("jni_shim:   -> Bundle.getString(\"%s\") = \"%s\"\n", key,
              value);
    return make_jstring(value);
  }
  if (methodID == &g_method_tags[MID_BUNDLE_GET_BYTE_ARRAY]) {
    void *key_str = NULL;
    if (g_static_args_override) {
      key_str = g_static_args[0];
    } else {
      va_list ap;
      va_start(ap, methodID);
      key_str = va_arg(ap, void *);
      va_end(ap);
    }
    const char *key = resolve_jstring(key_str);
    void *array = bundle_get_object(obj, key);
    jni_trace("jni_shim:   -> Bundle.getByteArray(\"%s\") = %p\n", key,
              array);
    return array ? array : alloc_fake_array(NULL, 0, 1, 1);
  }
  if (strcmp(id_name(methodID), "getString") == 0 ||
      strstr(id_sig(methodID), ")Ljava/lang/String;")) {
    debugPrintf("jni_shim:   -> %s = \"\"\n", id_name(methodID));
    return make_jstring("");
  }
  if (strcmp(id_name(methodID), "getByteArray") == 0 ||
      strstr(id_sig(methodID), ")[B")) {
    return alloc_fake_array(NULL, 0, 1, 1);
  }
  static int fake_obj;
  return &fake_obj;
}

static void *jni_CallObjectMethodV(void *env, void *obj, void *methodID,
                                   va_list args) {
  uintptr_t old_ret = g_jni_callsite_ret;
  g_jni_callsite_ret = (uintptr_t)__builtin_return_address(0);
  set_object_args_from_va(methodID, args);
  void *ret = jni_CallObjectMethod(env, obj, methodID);
  clear_static_args_override();
  g_jni_callsite_ret = old_ret;
  return ret;
}

static void *jni_CallObjectMethodA(void *env, void *obj, void *methodID,
                                   const jvalue *args) {
  uintptr_t old_ret = g_jni_callsite_ret;
  g_jni_callsite_ret = (uintptr_t)__builtin_return_address(0);
  set_object_args_from_jvalue(methodID, args);
  void *ret = jni_CallObjectMethod(env, obj, methodID);
  clear_static_args_override();
  g_jni_callsite_ret = old_ret;
  return ret;
}

/* NewObject (index 28/29/30) - Paddleboat constrói GameControllerManager Java;
 * NULL aqui = init -2002. Devolvemos objeto fake não-nulo. */
static void *jni_NewObject(void *env, void *clazz, void *methodID, ...) {
  (void)env;
  const char *class_name = resolve_fake_class(clazz);
  int kind = methodID == &g_method_tags[MID_BUNDLE_CTOR] ||
                     strcmp(class_name, "android/os/Bundle") == 0
                 ? OBJ_BUNDLE
                 : OBJ_GENERIC;
  void *obj = alloc_fake_object(kind);
  debugPrintf("jni_shim: NewObject(%s, mid=%p) -> %p\n", class_name,
              methodID, obj);
  return obj;
}

/* ---- Arrays fake genéricos (int/float) p/ JNI Region/Elements ---- */
#define MAX_FAKE_ARRAYS 16
static struct {
  void *handle;
  void *data; /* int32, float, byte ou jobject */
  int len;
  int elem_size;
  int owned;
} g_fake_arrays[MAX_FAKE_ARRAYS];
static char g_fake_array_handles[MAX_FAKE_ARRAYS];
static int g_fake_array_count = 0;

static void *alloc_fake_array(const void *data, int len, int elem_size,
                              int copy_data) {
  if (len < 0)
    len = 0;
  if (elem_size <= 0)
    elem_size = 1;
  if (g_fake_array_count >= MAX_FAKE_ARRAYS) g_fake_array_count = 0;
  int i = g_fake_array_count++;
  if (g_fake_arrays[i].owned && g_fake_arrays[i].data)
    free(g_fake_arrays[i].data);
  g_fake_arrays[i].handle = &g_fake_array_handles[i];
  g_fake_arrays[i].len = len;
  g_fake_arrays[i].elem_size = elem_size;
  g_fake_arrays[i].owned = copy_data;
  if (copy_data) {
    size_t bytes = (size_t)(len > 0 ? len : 1) * (size_t)elem_size;
    g_fake_arrays[i].data = calloc(1, bytes);
    if (data && g_fake_arrays[i].data)
      memcpy(g_fake_arrays[i].data, data, (size_t)len * (size_t)elem_size);
  } else {
    g_fake_arrays[i].data = (void *)data;
  }
  return g_fake_arrays[i].handle;
}

void *jni_shim_make_array(const void *data, int len) {
  return alloc_fake_array(data, len, sizeof(jint), 0);
}

static int find_fake_array(void *h) {
  for (int i = 0; i < g_fake_array_count; i++)
    if (g_fake_arrays[i].handle == h) return i;
  return -1;
}

/* CallBooleanMethod (index 49) */
static unsigned char jni_CallBooleanMethod(void *env, void *obj,
                                           void *methodID, ...) {
  (void)env;
  log_call_id("CallBooleanMethod", methodID);
  if (methodID == &g_method_tags[MID_BUNDLE_CONTAINS_KEY]) {
    void *key_str = NULL;
    if (g_static_args_override) {
      key_str = g_static_args[0];
    } else {
      va_list ap;
      va_start(ap, methodID);
      key_str = va_arg(ap, void *);
      va_end(ap);
    }
    return bundle_has_key(obj, resolve_jstring(key_str)) ? 1 : 0;
  }
  if (methodID == &g_method_tags[MID_BUNDLE_GET_BOOLEAN]) {
    void *key_str = NULL;
    unsigned char default_value = 0;
    if (g_static_args_override) {
      key_str = g_static_args[0];
      default_value = (unsigned char)(intptr_t)g_static_args[1];
    } else {
      va_list ap;
      va_start(ap, methodID);
      key_str = va_arg(ap, void *);
      if (strstr(id_sig(methodID), "Ljava/lang/String;Z"))
        default_value = va_arg(ap, int) ? 1 : 0;
      va_end(ap);
    }
    unsigned char value =
        bundle_get_boolean(obj, resolve_jstring(key_str), default_value);
    jni_trace("jni_shim:   -> Bundle.getBoolean(\"%s\") = %d\n",
              resolve_jstring(key_str), value ? 1 : 0);
    return value ? 1 : 0;
  }
  return 0;
}

static unsigned char jni_CallBooleanMethodV(void *env, void *obj,
                                            void *methodID, va_list args) {
  uintptr_t old_ret = g_jni_callsite_ret;
  g_jni_callsite_ret = (uintptr_t)__builtin_return_address(0);
  set_object_args_from_va(methodID, args);
  unsigned char ret = jni_CallBooleanMethod(env, obj, methodID);
  clear_static_args_override();
  g_jni_callsite_ret = old_ret;
  return ret;
}

static unsigned char jni_CallBooleanMethodA(void *env, void *obj,
                                            void *methodID,
                                            const jvalue *args) {
  uintptr_t old_ret = g_jni_callsite_ret;
  g_jni_callsite_ret = (uintptr_t)__builtin_return_address(0);
  set_object_args_from_jvalue(methodID, args);
  unsigned char ret = jni_CallBooleanMethod(env, obj, methodID);
  clear_static_args_override();
  g_jni_callsite_ret = old_ret;
  return ret;
}

/* CallIntMethod (index 61) */
static jint jni_CallIntMethod(void *env, void *obj, void *methodID, ...) {
  (void)env;
  log_call_id("CallIntMethod", methodID);
  if (methodID == &g_method_tags[MID_BATTERY_STATUS])
    return 2; /* BATTERY_STATUS_CHARGING — sem power-save/cap de fps */
  if (methodID == &g_method_tags[MID_BUNDLE_GET_INT]) {
    void *key_str = NULL;
    jint default_value = 0;
    if (g_static_args_override) {
      key_str = g_static_args[0];
      default_value = (jint)(intptr_t)g_static_args[1];
    } else {
      va_list ap;
      va_start(ap, methodID);
      key_str = va_arg(ap, void *);
      if (strstr(id_sig(methodID), "Ljava/lang/String;I"))
        default_value = va_arg(ap, int);
      va_end(ap);
    }
    jint value = bundle_get_int(obj, resolve_jstring(key_str), default_value);
    jni_trace("jni_shim:   -> Bundle.getInt(\"%s\") = %d\n",
              resolve_jstring(key_str), (int)value);
    return value;
  }
  return 0;
}

static jint jni_CallIntMethodV(void *env, void *obj, void *methodID,
                               va_list args) {
  uintptr_t old_ret = g_jni_callsite_ret;
  g_jni_callsite_ret = (uintptr_t)__builtin_return_address(0);
  set_object_args_from_va(methodID, args);
  jint ret = jni_CallIntMethod(env, obj, methodID);
  clear_static_args_override();
  g_jni_callsite_ret = old_ret;
  return ret;
}

static jint jni_CallIntMethodA(void *env, void *obj, void *methodID,
                               const jvalue *args) {
  uintptr_t old_ret = g_jni_callsite_ret;
  g_jni_callsite_ret = (uintptr_t)__builtin_return_address(0);
  set_object_args_from_jvalue(methodID, args);
  jint ret = jni_CallIntMethod(env, obj, methodID);
  clear_static_args_override();
  g_jni_callsite_ret = old_ret;
  return ret;
}

static jint static_int_value(void *methodID) {
  if (methodID == &g_method_tags[MID_GET_WINDOW_WIDTH] ||
      methodID == &g_method_tags[MID_GET_DEVICE_WIDTH] ||
      methodID == &g_method_tags[MID_GET_RESOLUTION_X])
    return dys_screen_w > 0 ? dys_screen_w : 1280;
  if (methodID == &g_method_tags[MID_GET_WINDOW_HEIGHT] ||
      methodID == &g_method_tags[MID_GET_DEVICE_HEIGHT] ||
      methodID == &g_method_tags[MID_GET_RESOLUTION_Y])
    return dys_screen_h > 0 ? dys_screen_h : 720;
  if (methodID == &g_method_tags[MID_CHECK_HDMI_STATE])
    return 1;
  if (methodID == &g_method_tags[MID_IS_KEYBOARD_VISIBLE])
    return 0;
  if (methodID == &g_method_tags[MID_GET_UPTIME])
    return 60;
  if (methodID == &g_method_tags[MID_INIT_CHECK_CONNECTION]) {
    const char *v = getenv("TASM2_CONNECTION_TYPE");
    return (v && *v) ? (jint)strtol(v, NULL, 10) : 0;
  }
  if (methodID == &g_method_tags[MID_GET_GOOGLE_AD_ID_STATUS])
    return 0;
  return 0;
}

/* CallFloatMethod (index 55-57) — sem isto o retorno float era LIXO em s0
 * (getBatteryLevel lia qualquer coisa -> engine entrava em power-save 30fps) */
static JNI_SF float jni_CallFloatMethod(void *env, void *obj, void *methodID, ...) {
  (void)env;
  (void)obj;
  log_call_id("CallFloatMethod", methodID);
  if (methodID == &g_method_tags[MID_BATTERY_LEVEL])
    return 100.0f; /* 100% (escala 0..1 ou 0..100, ambas acima do threshold) */
  return 0.0f;
}

/* CallVoidMethod (index 94) */
static void jni_CallVoidMethod(void *env, void *obj, void *methodID, ...) {
  (void)env;
  log_call_id("CallVoidMethod", methodID);
  if (methodID == &g_method_tags[MID_BUNDLE_CLEAR]) {
    bundle_clear(obj);
    return;
  }
  if (methodID == &g_method_tags[MID_BUNDLE_PUT_STRING] ||
      methodID == &g_method_tags[MID_BUNDLE_PUT_INT] ||
      methodID == &g_method_tags[MID_BUNDLE_PUT_LONG] ||
      methodID == &g_method_tags[MID_BUNDLE_PUT_BOOLEAN] ||
      methodID == &g_method_tags[MID_BUNDLE_PUT_BYTE_ARRAY] ||
      methodID == &g_method_tags[MID_BUNDLE_PUT_ALL]) {
    void *key_str = NULL;
    void *obj_value = NULL;
    jint int_value = 0;
    long long long_value = 0;
    if (g_static_args_override) {
      key_str = g_static_args[0];
      obj_value = g_static_args[1];
      int_value = (jint)(intptr_t)g_static_args[1];
      long_value = g_static_long_args[1];
    } else {
      va_list ap;
      va_start(ap, methodID);
      key_str = va_arg(ap, void *);
      if (methodID == &g_method_tags[MID_BUNDLE_PUT_ALL])
        obj_value = key_str;
      else if (methodID == &g_method_tags[MID_BUNDLE_PUT_INT] ||
          methodID == &g_method_tags[MID_BUNDLE_PUT_BOOLEAN])
        int_value = va_arg(ap, int);
      else if (methodID == &g_method_tags[MID_BUNDLE_PUT_LONG])
        long_value = va_arg(ap, long long);
      else
        obj_value = va_arg(ap, void *);
      va_end(ap);
    }
    const char *key = resolve_jstring(key_str);
    if (methodID == &g_method_tags[MID_BUNDLE_PUT_STRING])
      bundle_put_string(obj, key, resolve_jstring(obj_value));
    else if (methodID == &g_method_tags[MID_BUNDLE_PUT_INT])
      bundle_put_int(obj, key, int_value);
    else if (methodID == &g_method_tags[MID_BUNDLE_PUT_LONG])
      bundle_put_long(obj, key, long_value);
    else if (methodID == &g_method_tags[MID_BUNDLE_PUT_BOOLEAN])
      bundle_put_boolean(obj, key, int_value ? 1 : 0);
    else if (methodID == &g_method_tags[MID_BUNDLE_PUT_BYTE_ARRAY])
      bundle_put_object(obj, key, obj_value);
    else if (methodID == &g_method_tags[MID_BUNDLE_PUT_ALL])
      bundle_copy(obj, g_static_args_override ? key_str : obj_value);
    return;
  }
}

static void jni_CallVoidMethodV(void *env, void *obj, void *methodID,
                                va_list args) {
  uintptr_t old_ret = g_jni_callsite_ret;
  g_jni_callsite_ret = (uintptr_t)__builtin_return_address(0);
  set_object_args_from_va(methodID, args);
  jni_CallVoidMethod(env, obj, methodID);
  clear_static_args_override();
  g_jni_callsite_ret = old_ret;
}

static void jni_CallVoidMethodA(void *env, void *obj, void *methodID,
                                const jvalue *args) {
  uintptr_t old_ret = g_jni_callsite_ret;
  g_jni_callsite_ret = (uintptr_t)__builtin_return_address(0);
  set_object_args_from_jvalue(methodID, args);
  jni_CallVoidMethod(env, obj, methodID);
  clear_static_args_override();
  g_jni_callsite_ret = old_ret;
}

static const char *tasm2_data_root(void) {
  const char *root = getenv("TASM2_DATA");
  return (root && *root) ? root : "/storage/roms/ports/tasm2";
}

static void *make_asset_byte_array(const char *asset_name) {
  char path[1024];
  const char *name = asset_name ? asset_name : "";
  while (*name == '/')
    name++;
  snprintf(path, sizeof(path), "%s/assets/%s", tasm2_data_root(), name);

  FILE *f = fopen(path, "rb");
  if (!f) {
    jni_trace("jni_shim: asset %s nao encontrado\n", path);
    return alloc_fake_array(NULL, 0, 1, 1);
  }
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return alloc_fake_array(NULL, 0, 1, 1);
  }
  long size = ftell(f);
  if (size < 0 || size > 1024 * 1024) {
    fclose(f);
    return alloc_fake_array(NULL, 0, 1, 1);
  }
  rewind(f);
  unsigned char *buf = size > 0 ? malloc((size_t)size) : NULL;
  size_t got = size > 0 && buf ? fread(buf, 1, (size_t)size, f) : 0;
  fclose(f);
  void *array = alloc_fake_array(buf, (int)got, 1, 1);
  free(buf);
  jni_trace("jni_shim: asset %s -> %d bytes\n", path, (int)got);
  return array;
}

/* CallStaticObjectMethod (index 113) */
static void *jni_CallStaticObjectMethod(void *env, void *clazz,
                                        void *methodID, ...) {
  (void)env;
  (void)clazz;
  LOG_STATIC_CALL("CallStaticObjectMethod", methodID);

  if (methodID == &g_method_tags[MID_GET_STORAGE_DIR]) {
    debugPrintf("jni_shim: CallStaticObjectMethod -> getStorageDir = \"/storage/roms/ports/tasm2/\"\n");
    return make_jstring("/storage/roms/ports/tasm2/");
  }
  if (methodID == &g_method_tags[MID_GET_PACK_NAME]) {
    debugPrintf(
        "jni_shim: CallStaticObjectMethod -> getPackName = \"%s\"\n",
        g_package_name);
    return make_jstring(g_package_name);
  }
  if (methodID == &g_method_tags[MID_GET_PACKAGE]) {
    debugPrintf("jni_shim: CallStaticObjectMethod -> getPackage = \"%s\"\n",
                g_package_name);
    return make_jstring(g_package_name);
  }
  if (methodID == &g_method_tags[MID_GET_SAVE_FOLDER]) {
    return make_jstring("/storage/roms/ports/tasm2/files/");
  }
  if (methodID == &g_method_tags[MID_GET_SD_FOLDER]) {
    return make_jstring("/storage/emulated/0/Android/data/com.gameloft.android.ANMP.GloftASHM/files/");
  }
  if (methodID == &g_method_tags[MID_GET_GAME_NAME]) {
    return make_jstring("ANMP.GloftASHM");
  }
  if (methodID == &g_method_tags[MID_GET_INJECTED_IGP] ||
      methodID == &g_method_tags[MID_GET_INJECTED_SERIAL_KEY]) {
    return make_jstring("");
  }
  if (methodID == &g_method_tags[MID_GET_APK_PATH]) {
    return make_jstring("/storage/roms/ports/tasm2/base.apk");
  }
  if (methodID == &g_method_tags[MID_GET_ASSET_AS_STRING]) {
    void *asset = NULL;
    if (g_static_args_override) {
      asset = g_static_args[0];
    } else {
      va_list ap;
      va_start(ap, methodID);
      asset = va_arg(ap, void *);
      va_end(ap);
    }
    return make_asset_byte_array(resolve_jstring(asset));
  }
  if (methodID == &g_method_tags[MID_IAP_GET_DATA]) {
    void *request = NULL;
    if (g_static_args_override) {
      request = g_static_args[0];
    } else {
      va_list ap;
      va_start(ap, methodID);
      request = va_arg(ap, void *);
      va_end(ap);
    }
    void *response = alloc_fake_object(OBJ_BUNDLE);
    int op = bundle_get_int(request, "O", -1);
    bundle_copy(response, request);
    bundle_dump(request, "IAP request");
    bundle_seed_iap_response(response, op);
    bundle_dump(response, "IAP response");
    jni_trace("jni_shim:   -> InAppBilling.getData(%p) = %p\n",
              request, response);
    return response;
  }
  if (methodID == &g_method_tags[MID_NATIVE_GET_PREFERENCE]) {
    void *request = NULL;
    if (g_static_args_override) {
      request = g_static_args[0];
    } else {
      va_list ap;
      va_start(ap, methodID);
      request = va_arg(ap, void *);
      va_end(ap);
    }
    const char *default_value =
        bundle_get_string(request, "npDefaultValue", "");
    void *response = alloc_fake_object(OBJ_BUNDLE);
    bundle_copy(response, request);
    bundle_put_string(response, "npResult", default_value);
    bundle_put_int(response, "npResultCode", 0);
    jni_trace("jni_shim:   -> nativeGetPreference(%p) = %p result=\"%s\"\n",
              request, response, default_value);
    return response;
  }
  if (methodID == &g_method_tags[MID_GET_META_DATA_VALUE]) {
    void *key_str = NULL;
    if (g_static_args_override) {
      key_str = g_static_args[0];
    } else {
      va_list ap;
      va_start(ap, methodID);
      key_str = va_arg(ap, void *);
      va_end(ap);
    }
    const char *key = resolve_jstring(key_str);
    const char *value = "";
    if (strcmp(key, "com.facebook.sdk.ApplicationId") == 0)
      value = "446584868786398";
    else if (strcmp(key, "com.google.android.gms.games.APP_ID") == 0)
      value = "756977471932";
    else if (strcmp(key, "com.google.android.gms.version") == 0)
      value = "8487000";
    else if (strcmp(key, "com.google.android.gms.analytics.globalConfigResource") == 0)
      value = "res/xml/global_config.xml";
    else if (strcmp(key, "CHANNEL_ID") == 0)
      value = "55629";
    else if (strcmp(key, "com.samsung.android.infinitedisplay.supported") == 0)
      value = "yes";
    else if (strcmp(key, "android.max_aspect") == 0)
      value = "2.500000";
    jni_trace("jni_shim:   -> getMetaDataValue(\"%s\") = \"%s\"\n", key,
              value);
    return make_jstring(value);
  }
  if (methodID == &g_method_tags[MID_GET_PREFERENCE_STRING]) {
    void *key_str = NULL;
    void *default_str = NULL;
    if (g_static_args_override) {
      key_str = g_static_args[0];
      default_str = g_static_args[1];
    } else {
      va_list ap;
      va_start(ap, methodID);
      key_str = va_arg(ap, void *);
      default_str = va_arg(ap, void *);
      va_end(ap);
    }
    const char *key = resolve_jstring(key_str);
    const char *fallback = resolve_jstring(default_str);
    const char *value = fallback;
    if (strcmp(key, "SDFolder") == 0)
      value = "/storage/emulated/0/Android/data/com.gameloft.android.ANMP.GloftASHM/files";
    jni_trace("jni_shim:   -> getPreferenceString(\"%s\") = \"%s\"\n", key,
              value);
    return make_jstring(value);
  }
  if (methodID == &g_method_tags[MID_GET_CONTEXT]) {
    static void *fake_context;
    if (!fake_context)
      fake_context = alloc_fake_object(OBJ_CONTEXT);
    return fake_context;
  }
  if (methodID == &g_method_tags[MID_GET_USER_AGENT]) {
    return make_jstring("Mozilla/5.0 (Linux; Android 4.4; NextOS)");
  }
  if (methodID == &g_method_tags[MID_GET_HDMI_NAME]) {
    return make_jstring("HDMI");
  }
  if (methodID == &g_method_tags[MID_GET_UDID] ||
      methodID == &g_method_tags[MID_GET_HIDFV] ||
      methodID == &g_method_tags[MID_GET_ANDROID_ID]) {
    return make_jstring("nextos-tasm2-device");
  }
  if (methodID == &g_method_tags[MID_D1] ||
      methodID == &g_method_tags[MID_GET_SERIAL] ||
      methodID == &g_method_tags[MID_GET_SERIAL_NO] ||
      methodID == &g_method_tags[MID_GET_DEVICE_IMEI] ||
      methodID == &g_method_tags[MID_GET_GLDID] ||
      methodID == &g_method_tags[MID_RETRIEVE_CPU_SERIAL]) {
    return make_jstring("nextos-tasm2-device");
  }
  if (methodID == &g_method_tags[MID_GET_MAC_ADDRESS]) {
    return make_jstring("02:00:00:00:00:00");
  }
  if (methodID == &g_method_tags[MID_GET_HIDFV_VERSION]) {
    return make_jstring("1");
  }
  if (methodID == &g_method_tags[MID_GET_GOOGLE_AD_ID]) {
    return make_jstring("00000000-0000-0000-0000-000000000000");
  }
  if (methodID == &g_method_tags[MID_GET_COUNTRY]) {
    return make_jstring("US");
  }
  if (methodID == &g_method_tags[MID_RETRIEVE_DEVICE_COUNTRY] ||
      methodID == &g_method_tags[MID_RETRIEVE_DEVICE_REGION]) {
    return make_jstring("US");
  }
  if (methodID == &g_method_tags[MID_RETRIEVE_DEVICE_LANGUAGE]) {
    return make_jstring("en");
  }
  if (methodID == &g_method_tags[MID_RETRIEVE_DEVICE_CARRIER]) {
    return make_jstring("");
  }
  if (methodID == &g_method_tags[MID_GET_MANUFACTURER]) {
    return make_jstring("NextOS");
  }
  if (methodID == &g_method_tags[MID_GET_PHONE_MANUFACTURER]) {
    return make_jstring("NextOS");
  }
  if (methodID == &g_method_tags[MID_GET_DEVICE_NAME]) {
    return make_jstring("Amlogic");
  }
  if (methodID == &g_method_tags[MID_GET_DEVICE_NAME_JAVA] ||
      methodID == &g_method_tags[MID_GET_PHONE_MODEL] ||
      methodID == &g_method_tags[MID_GET_PHONE_DEVICE] ||
      methodID == &g_method_tags[MID_GET_PHONE_PRODUCT]) {
    return make_jstring("Amlogic");
  }
  if (methodID == &g_method_tags[MID_GET_DEVICE_FIRMWARE]) {
    return make_jstring("3.14.79");
  }
  if (methodID == &g_method_tags[MID_GET_DEVICE_FIRMWARE_JAVA]) {
    return make_jstring("3.14.79");
  }
  if (methodID == &g_method_tags[MID_GET_DEVICE_LANGUAGE]) {
    return make_jstring("en");
  }
  if (methodID == &g_method_tags[MID_GET_SHARED_VALUE]) {
    void *key_str = NULL;
    if (g_static_args_override) {
      key_str = g_static_args[0];
    } else {
      va_list ap;
      va_start(ap, methodID);
      key_str = va_arg(ap, void *);
      va_end(ap);
    }
    const char *key = resolve_jstring(key_str);
    const char *value = shared_value_get(key);
    jni_trace("jni_shim:   -> getSharedValue(\"%s\") = \"%s\"\n", key,
              value ? value : "(null)");
    /* Valor ausente: devolver STRING VAZIA, nao jstring nula. O GAIA nativo
     * constroi std::string(value) direto — com null-jstring o GetStringUTFChars
     * volta NULL e o ctor de std::string estoura (SIGSEGV em 0xc8e2fc). Com ""
     * ele constroi string vazia, ve que esta vazia e GERA um GLUID fresco
     * (fluxo de device novo), salvando via setSharedValue. */
    return make_jstring(value ? value : "");
  }
  if (methodID == &g_method_tags[MID_GET_RESOURCE] ||
      methodID == &g_method_tags[MID_GET_KEYBOARD_TEXT]) {
    return alloc_fake_array(NULL, 0, 1, 1);
  }
  if (methodID == &g_method_tags[MID_RETRIEVE_BARRELS] ||
      methodID == &g_method_tags[MID_GET_GLUID]) {
    static jint one[1] = {0};
    return alloc_fake_array(one, 1, sizeof(jint), 1);
  }
  /* Locale.getDefault() -> objeto Locale fake não-nulo (p/ getLanguage depois) */
  if (methodID == &g_method_tags[MID_GET_DEFAULT_LOCALE]) {
    debugPrintf("jni_shim: CallStaticObjectMethod -> Locale.getDefault (fake)\n");
    return make_jstring("locale");
  }

  debugPrintf("jni_shim: CallStaticObjectMethod(%s%s, mid=%p) -> fake\n",
              id_name(methodID), id_sig(methodID), methodID);
  if (strstr(id_sig(methodID), ")Ljava/lang/String;"))
    return make_jstring("");
  if (strstr(id_sig(methodID), ")[B"))
    return alloc_fake_array(NULL, 0, 1, 1);
  if (strstr(id_sig(methodID), ")Landroid/os/Bundle;"))
    return alloc_fake_object(OBJ_BUNDLE);
  static int fake_result;
  return &fake_result;
}

static int static_object_method_arg_count(void *methodID) {
  if (methodID == &g_method_tags[MID_GET_PREFERENCE_STRING])
    return 2;
  if (methodID == &g_method_tags[MID_GET_ASSET_AS_STRING] ||
      methodID == &g_method_tags[MID_GET_SHARED_VALUE] ||
      methodID == &g_method_tags[MID_GET_META_DATA_VALUE] ||
      methodID == &g_method_tags[MID_IAP_GET_DATA] ||
      methodID == &g_method_tags[MID_NATIVE_GET_PREFERENCE])
    return 1;
  return 0;
}

static void *jni_CallStaticObjectMethodV(void *env, void *clazz,
                                         void *methodID, va_list args) {
  uintptr_t old_ret = g_jni_callsite_ret;
  g_jni_callsite_ret = (uintptr_t)__builtin_return_address(0);
  int argc = static_object_method_arg_count(methodID);
  va_list ap;
  va_copy(ap, args);
  g_static_args_override = 1;
  for (int i = 0; i < argc && i < 4; i++)
    g_static_args[i] = va_arg(ap, void *);
  va_end(ap);
  void *ret = jni_CallStaticObjectMethod(env, clazz, methodID);
  clear_static_args_override();
  g_jni_callsite_ret = old_ret;
  return ret;
}

static void *jni_CallStaticObjectMethodA(void *env, void *clazz,
                                         void *methodID,
                                         const jvalue *args) {
  uintptr_t old_ret = g_jni_callsite_ret;
  g_jni_callsite_ret = (uintptr_t)__builtin_return_address(0);
  int argc = static_object_method_arg_count(methodID);
  g_static_args_override = 1;
  for (int i = 0; i < argc && i < 4; i++)
    g_static_args[i] = args ? args[i].l : NULL;
  void *ret = jni_CallStaticObjectMethod(env, clazz, methodID);
  clear_static_args_override();
  g_jni_callsite_ret = old_ret;
  return ret;
}

/* CallStaticBooleanMethod (index 124) */
static unsigned char jni_CallStaticBooleanMethod(void *env, void *clazz,
                                                 void *methodID, ...) {
  (void)env;
  (void)clazz;
  LOG_STATIC_CALL("CallStaticBooleanMethod", methodID);
  if (methodID == &g_method_tags[MID_SET_CURRENT_CONTEXT]) {
    int context_id = 0;
    if (g_static_args_override) {
      context_id = (int)(intptr_t)g_static_args[0];
    } else {
      va_list ap;
      va_start(ap, methodID);
      context_id = va_arg(ap, int);
      va_end(ap);
    }
    int ok = egl_shim_make_bootstrap_current();
    int ret = ok || context_id > 0;
    jni_trace("jni_shim:   -> setCurrentContext(%d) = %d [egl=%d]\n",
              context_id, ret, ok);
    return ret ? 1 : 0;
  }
  if (methodID == &g_method_tags[MID_IS_SHARED_VALUE]) {
    void *key_str = NULL;
    if (g_static_args_override) {
      key_str = g_static_args[0];
    } else {
      va_list ap;
      va_start(ap, methodID);
      key_str = va_arg(ap, void *);
      va_end(ap);
    }
    const char *key = resolve_jstring(key_str);
    int has = shared_value_has(key);
    jni_trace("jni_shim:   -> isSharedValue(\"%s\") = %d\n", key, has);
    return has ? 1 : 0;
  }
  const char *name = id_name(methodID);
  if (strstr(name, "LoggedIn")) {
    int logged_in = getenv("TASM2_GAMEAPI_LOGGED_IN") ? 1 : 0;
    jni_trace("jni_shim:   -> %s = %d [gameapi]\n", name, logged_in);
    return logged_in ? 1 : 0;
  }
  if (strstr(name, "Permission") || strstr(name, "Dialog") ||
      strstr(name, "KeyboardVisible") ||
      strstr(name, "playVideo") || strstr(name, "LaunchVideo")) {
    jni_trace("jni_shim:   -> %s offline/false\n", name);
    return 0;
  }
  if (strstr(name, "genericUnzipArchive") ||
      strstr(name, "removeDirectoryRecursively") ||
      strstr(name, "hasTouch") || strstr(name, "HasTouch")) {
    jni_trace("jni_shim:   -> %s success/true\n", name);
    return 1;
  }
  debugPrintf("jni_shim:   -> 1\n");
  // Return true for hasTouchScreen — prevents game from managing
  // Shield gamepad button layouts that don't exist in the OBB.
  return 1;
}

static int static_boolean_method_arg_count(void *methodID) {
  if (methodID == &g_method_tags[MID_SET_CURRENT_CONTEXT] ||
      methodID == &g_method_tags[MID_IS_SHARED_VALUE])
    return 1;
  return 0;
}

static unsigned char jni_CallStaticBooleanMethodV(void *env, void *clazz,
                                                  void *methodID,
                                                  va_list args) {
  uintptr_t old_ret = g_jni_callsite_ret;
  g_jni_callsite_ret = (uintptr_t)__builtin_return_address(0);
  int argc = static_boolean_method_arg_count(methodID);
  va_list ap;
  va_copy(ap, args);
  g_static_args_override = 1;
  for (int i = 0; i < argc && i < 4; i++) {
    if (methodID == &g_method_tags[MID_SET_CURRENT_CONTEXT])
      g_static_args[i] = (void *)(intptr_t)va_arg(ap, int);
    else
      g_static_args[i] = va_arg(ap, void *);
  }
  va_end(ap);
  unsigned char ret = jni_CallStaticBooleanMethod(env, clazz, methodID);
  clear_static_args_override();
  g_jni_callsite_ret = old_ret;
  return ret;
}

static unsigned char jni_CallStaticBooleanMethodA(void *env, void *clazz,
                                                  void *methodID,
                                                  const jvalue *args) {
  uintptr_t old_ret = g_jni_callsite_ret;
  g_jni_callsite_ret = (uintptr_t)__builtin_return_address(0);
  int argc = static_boolean_method_arg_count(methodID);
  g_static_args_override = 1;
  for (int i = 0; i < argc && i < 4; i++) {
    if (methodID == &g_method_tags[MID_SET_CURRENT_CONTEXT])
      g_static_args[i] = (void *)(intptr_t)(args ? args[i].i : 0);
    else
      g_static_args[i] = args ? args[i].l : NULL;
  }
  unsigned char ret = jni_CallStaticBooleanMethod(env, clazz, methodID);
  clear_static_args_override();
  g_jni_callsite_ret = old_ret;
  return ret;
}

/* CallStaticIntMethod (index 136) */
static jint jni_CallStaticIntMethod(void *env, void *clazz, void *methodID,
                                    ...) {
  (void)env;
  (void)clazz;
  LOG_STATIC_CALL("CallStaticIntMethod", methodID);
  return static_int_value(methodID);
}

static JNI_SF float jni_CallStaticFloatMethod(void *env, void *clazz,
                                              void *methodID, ...) {
  (void)env;
  (void)clazz;
  LOG_STATIC_CALL("CallStaticFloatMethod", methodID);
  if (methodID == &g_method_tags[MID_GET_CPU_MAX])
    return 1600.0f;
  if (methodID == &g_method_tags[MID_GET_CPU_CURRENT])
    return 1600.0f;
  if (methodID == &g_method_tags[MID_GET_MAX_RAM])
    return 1024.0f;
  if (methodID == &g_method_tags[MID_GET_FREE_DISK])
    return 30000.0f;
  if (methodID == &g_method_tags[MID_GET_FREE_RAM])
    return 512.0f;
  return 0.0f;
}

typedef void (*jni_native2_fn)(void *env, void *clazz);
typedef void (*gameapi_notify_fn)(void *env, void *clazz, unsigned char logged,
                                  void *account);

static int env_flag_enabled(const char *name) {
  const char *v = getenv(name);
  return v && *v && strcmp(v, "0") != 0;
}

static void gameapi_native_init_once(void *env, void *clazz) {
  if (g_gameapi_native_init_done)
    return;
  g_gameapi_native_init_done = 1;

  if (env_flag_enabled("TASM2_GAMEAPI_PLATFORM_INIT")) {
    uintptr_t platform_init =
        so_find_addr_safe("Java_com_gameloft_GLSocialLib_PlatformAndroid_nativeInit");
    if (platform_init) {
      ((jni_native2_fn)platform_init)(env, clazz);
      jni_trace("jni_shim:   -> PlatformAndroid.nativeInit [gameapi]\n");
    }
  }

  uintptr_t gameapi_init = so_find_addr_safe(
      "Java_com_gameloft_GLSocialLib_GameAPI_GameAPIAndroidGLSocialLib_nativeInit");
  if (gameapi_init) {
    ((jni_native2_fn)gameapi_init)(env, clazz);
    jni_trace("jni_shim:   -> GameAPIAndroidGLSocialLib.nativeInit\n");
  }
}

static void maybe_gameapi_native_callbacks(void *env, void *clazz,
                                           const char *method_name) {
  if (!env_flag_enabled("TASM2_GAMEAPI_NATIVE_CALLBACKS"))
    return;
  if (!method_name ||
      (strcmp(method_name, "InitGameAPI") != 0 &&
       strcmp(method_name, "ConnectToService") != 0))
    return;
  if (g_gameapi_native_callback_done &&
      !env_flag_enabled("TASM2_GAMEAPI_CALLBACK_REPEAT"))
    return;
  g_gameapi_native_callback_done = 1;

  gameapi_native_init_once(env, clazz);

  const char *account = getenv("TASM2_GAMEAPI_ACCOUNT");
  if (!account || !*account)
    account = "nextos-tasm2";
  void *account_str = make_jstring(account);

  uintptr_t notify = so_find_addr_safe(
      "Java_com_gameloft_GLSocialLib_GameAPI_GameAPIAndroidGLSocialLib_nativeGameAPINotifyAuthChanges");
  if (notify) {
    ((gameapi_notify_fn)notify)(env, clazz, 1, account_str);
    jni_trace("jni_shim:   -> nativeGameAPINotifyAuthChanges(logged=1)\n");
  }

  if (env_flag_enabled("TASM2_GAMEAPI_NATIVE_COMPLETE")) {
    uintptr_t complete = so_find_addr_safe(
        "Java_com_gameloft_GLSocialLib_GameAPI_GameAPIAndroidGLSocialLib_nativeGameAPIComplete");
    if (complete) {
      ((jni_native2_fn)complete)(env, clazz);
      jni_trace("jni_shim:   -> nativeGameAPIComplete\n");
    }
  }
}

/* CallStaticVoidMethod (index 145) */
static void jni_CallStaticVoidMethod(void *env, void *clazz, void *methodID,
                                     ...) {
  LOG_STATIC_CALL("CallStaticVoidMethod", methodID);
  if (methodID == &g_method_tags[MID_SET_SHARED_VALUE]) {
    void *key_str = NULL;
    void *value_str = NULL;
    if (g_static_args_override) {
      key_str = g_static_args[0];
      value_str = g_static_args[1];
    } else {
      va_list ap;
      va_start(ap, methodID);
      key_str = va_arg(ap, void *);
      value_str = va_arg(ap, void *);
      va_end(ap);
    }
    const char *key = resolve_jstring(key_str);
    const char *value = resolve_jstring(value_str);
    shared_value_set(key, value);
    jni_trace("jni_shim:   -> setSharedValue(\"%s\", \"%s\")\n", key,
              value);
    return;
  }
  if (methodID == &g_method_tags[MID_DELETE_SHARED_VALUE]) {
    void *key_str = NULL;
    if (g_static_args_override) {
      key_str = g_static_args[0];
    } else {
      va_list ap;
      va_start(ap, methodID);
      key_str = va_arg(ap, void *);
      va_end(ap);
    }
    const char *key = resolve_jstring(key_str);
    shared_value_delete(key);
    jni_trace("jni_shim:   -> deleteSharedValue(\"%s\")\n", key);
    return;
  }
  if (methodID == &g_method_tags[MID_NATIVE_SET_PREFERENCE]) {
    void *request = NULL;
    if (g_static_args_override) {
      request = g_static_args[0];
    } else {
      va_list ap;
      va_start(ap, methodID);
      request = va_arg(ap, void *);
      va_end(ap);
    }
    jni_trace("jni_shim:   -> nativeSetPreference(%p)\n", request);
    return;
  }
  maybe_gameapi_native_callbacks(env, clazz, id_name(methodID));
  if ((methodID == &g_method_tags[MID_SETUP_PATHS] ||
       methodID == &g_method_tags[MID_CREATE_VIEW]) &&
      g_setpaths_callback && getenv("TASM2_AUTO_SETPATHS")) {
    static int done;
    if (!done) {
      done = 1;
      void *data = make_jstring("/storage/emulated/0/Android/data/com.gameloft.android.ANMP.GloftASHM/files/");
      void *priv = make_jstring("/data/data/com.gameloft.android.ANMP.GloftASHM/");
      void *obb = make_jstring("/storage/roms/ports/tasm2/obb/");
      jni_trace("jni_shim:   -> chamando GL2JNILib.setPaths via setupPaths\n");
      g_setpaths_callback(env, clazz, data, priv, obb);
    }
  }
}

static int static_void_method_arg_count(void *methodID) {
  if (methodID == &g_method_tags[MID_SET_SHARED_VALUE])
    return 2;
  if (methodID == &g_method_tags[MID_DELETE_SHARED_VALUE])
    return 1;
  if (methodID == &g_method_tags[MID_NATIVE_SET_PREFERENCE])
    return 1;
  return 0;
}

static void jni_CallStaticVoidMethodV(void *env, void *clazz, void *methodID,
                                      va_list args) {
  uintptr_t old_ret = g_jni_callsite_ret;
  g_jni_callsite_ret = (uintptr_t)__builtin_return_address(0);
  int argc = static_void_method_arg_count(methodID);
  va_list ap;
  va_copy(ap, args);
  g_static_args_override = 1;
  for (int i = 0; i < argc && i < 4; i++)
    g_static_args[i] = va_arg(ap, void *);
  va_end(ap);
  jni_CallStaticVoidMethod(env, clazz, methodID);
  clear_static_args_override();
  g_jni_callsite_ret = old_ret;
}

static void jni_CallStaticVoidMethodA(void *env, void *clazz, void *methodID,
                                      const jvalue *args) {
  uintptr_t old_ret = g_jni_callsite_ret;
  g_jni_callsite_ret = (uintptr_t)__builtin_return_address(0);
  int argc = static_void_method_arg_count(methodID);
  g_static_args_override = 1;
  for (int i = 0; i < argc && i < 4; i++)
    g_static_args[i] = args ? args[i].l : NULL;
  jni_CallStaticVoidMethod(env, clazz, methodID);
  clear_static_args_override();
  g_jni_callsite_ret = old_ret;
}

/* GetStaticIntField (index 155) */
static jint jni_GetStaticIntField(void *env, void *clazz, void *fieldID) {
  (void)env;
  (void)clazz;

  if (fieldID == &g_method_tags[FID_OBB_VERSIONCODE]) {
    debugPrintf("jni_shim: GetStaticIntField -> OBB_VERSIONCODE = %d\n",
                g_obb_version);
    return g_obb_version;
  }
  debugPrintf("jni_shim: GetStaticIntField(%s %s%s, fid=%p) -> 0\n",
              id_kind(fieldID), id_name(fieldID), id_sig(fieldID), fieldID);
  return 0;
}

/* GetStaticObjectField (index 156) */
static void *jni_GetStaticObjectField(void *env, void *clazz, void *fieldID) {
  (void)env;
  (void)clazz;
  if (fieldID == &g_method_tags[FID_BUILD_MANUFACTURER])
    return make_jstring("NextOS");
  if (fieldID == &g_method_tags[FID_BUILD_MODEL])
    return make_jstring("Amlogic");
  if (fieldID == &g_method_tags[FID_BUILD_DEVICE])
    return make_jstring("nextos");
  if (fieldID == &g_method_tags[FID_BUILD_PRODUCT])
    return make_jstring("nextos");
  if (fieldID == &g_method_tags[FID_BUILD_VERSION_RELEASE])
    return make_jstring("4.4.2");
  debugPrintf("jni_shim: GetStaticObjectField(%s %s%s, fid=%p) -> \"\"\n",
              id_kind(fieldID), id_name(fieldID), id_sig(fieldID), fieldID);
  if (strstr(id_sig(fieldID), "Ljava/lang/String;"))
    return make_jstring("");
  static int fake;
  return &fake;
}

/* NewStringUTF (index 167) */
static void *jni_NewStringUTF(void *env, const char *str) {
  (void)env;
  debugPrintf("jni_shim: NewStringUTF(%s)\n", str ? str : "(null)");
  return make_jstring(str ? str : "");
}

/* GetStringUTFLength (index 168) */
static jint jni_GetStringUTFLength(void *env, void *jstr) {
  (void)env;
  const char *s = resolve_jstring(jstr);
  return (jint)strlen(s);
}

/* GetStringUTFChars (index 169) */
static const char *jni_GetStringUTFChars(void *env, void *jstr,
                                         void *isCopy) {
  (void)env;
  (void)isCopy;
  const char *s = resolve_jstring(jstr);
  debugPrintf("jni_shim: GetStringUTFChars -> \"%s\"\n", s);
  return s;
}

/* ReleaseStringUTFChars (index 170) */
static void jni_ReleaseStringUTFChars(void *env, void *jstr,
                                      const char *chars) {
  (void)env;
  (void)jstr;
  (void)chars;
}

/* Ref management */
static void *jni_NewGlobalRef(void *env, void *obj) {
  (void)env;
  return obj;
}
static void *jni_NewLocalRef(void *env, void *obj) {
  (void)env;
  return obj;
}
static void jni_DeleteGlobalRef(void *env, void *obj) {
  (void)env;
  (void)obj;
}
static void jni_DeleteLocalRef(void *env, void *obj) {
  (void)env;
  (void)obj;
}
static void *jni_GetObjectClass(void *env, void *obj) {
  (void)env;
  int idx = fake_object_index(obj);
  if (idx >= 0 && g_fake_objects[idx].kind == OBJ_BUNDLE)
    return remember_fake_class("android/os/Bundle");
  if (idx >= 0 && g_fake_objects[idx].kind == OBJ_CONTEXT)
    return remember_fake_class("android/content/Context");
  return remember_fake_class("java/lang/Object");
}

/* Exception handling */
static unsigned char jni_ExceptionCheck(void *env) {
  (void)env;
  return 0;
}
static void jni_ExceptionClear(void *env) { (void)env; }
static void jni_ExceptionDescribe(void *env) { (void)env; }
static void *jni_ExceptionOccurred(void *env) {
  (void)env;
  return 0;
}

static jint jni_GetJavaVM(void *env, void **vm) {
  (void)env;
  if (vm)
    *vm = &java_vm_ptr;
  return 0;
}

/* Array */
static jint jni_GetArrayLength(void *env, void *array) {
  (void)env;
  if (array == g_audio_params) return 2;
  int i = find_fake_array(array);
  if (i >= 0) return g_fake_arrays[i].len;
  return 0;
}
static void jni_GetIntArrayRegion(void *env, void *array, jint start, jint len,
                                  jint *buf) {
  (void)env;
  int i = find_fake_array(array);
  debugPrintf("jni_shim: GetIntArrayRegion(%p, %d, %d) idx=%d\n", array,
              (int)start, (int)len, i);
  if (i < 0 || !buf || g_fake_arrays[i].elem_size != (int)sizeof(jint)) return;
  const jint *d = (const jint *)g_fake_arrays[i].data;
  for (jint k = 0; k < len && (start + k) < g_fake_arrays[i].len; k++)
    buf[k] = d[start + k];
}
static void jni_GetFloatArrayRegion(void *env, void *array, jint start,
                                    jint len, float *buf) {
  (void)env;
  int i = find_fake_array(array);
  if (i < 0 || !buf || g_fake_arrays[i].elem_size != (int)sizeof(float)) return;
  const float *d = (const float *)g_fake_arrays[i].data;
  for (jint k = 0; k < len && (start + k) < g_fake_arrays[i].len; k++)
    buf[k] = d[start + k];
}
static jint *jni_GetIntArrayElements(void *env, void *array, unsigned char *isCopy) {
  (void)env;
  if (isCopy) *isCopy = 0;
  debugPrintf("jni_shim: GetIntArrayElements(%p)\n", array);
  if (array == g_audio_params) return g_audio_params;
  int i = find_fake_array(array);
  if (i >= 0 && g_fake_arrays[i].elem_size == (int)sizeof(jint))
    return (jint *)g_fake_arrays[i].data;
  static jint zeros[8];
  return zeros;
}
static void jni_ReleaseIntArrayElements(void *env, void *array, jint *elems,
                                        jint mode) {
  (void)env; (void)array; (void)elems; (void)mode;
}

static void *jni_NewObjectArray(void *env, jint len, void *clazz,
                                void *initial) {
  (void)env; (void)clazz;
  void *array = alloc_fake_array(NULL, len, (int)sizeof(void *), 1);
  int i = find_fake_array(array);
  if (i >= 0 && initial) {
    void **items = (void **)g_fake_arrays[i].data;
    for (jint k = 0; k < len; k++)
      items[k] = initial;
  }
  debugPrintf("jni_shim: NewObjectArray(%d) -> %p\n", (int)len, array);
  return array;
}

static void *jni_GetObjectArrayElement(void *env, void *array, jint index) {
  (void)env;
  int i = find_fake_array(array);
  if (i < 0 || g_fake_arrays[i].elem_size != (int)sizeof(void *))
    return NULL;
  if (index < 0 || index >= g_fake_arrays[i].len)
    return NULL;
  return ((void **)g_fake_arrays[i].data)[index];
}

static void jni_SetObjectArrayElement(void *env, void *array, jint index,
                                      void *value) {
  (void)env;
  int i = find_fake_array(array);
  if (i < 0 || g_fake_arrays[i].elem_size != (int)sizeof(void *))
    return;
  if (index < 0 || index >= g_fake_arrays[i].len)
    return;
  ((void **)g_fake_arrays[i].data)[index] = value;
}

static void *jni_NewByteArray(void *env, jint len) {
  (void)env;
  void *array = alloc_fake_array(NULL, len, 1, 1);
  debugPrintf("jni_shim: NewByteArray(%d) -> %p\n", (int)len, array);
  return array;
}

static void *jni_NewIntArray(void *env, jint len) {
  (void)env;
  void *array = alloc_fake_array(NULL, len, (int)sizeof(jint), 1);
  debugPrintf("jni_shim: NewIntArray(%d) -> %p\n", (int)len, array);
  return array;
}

static unsigned char *jni_GetByteArrayElements(void *env, void *array,
                                               unsigned char *isCopy) {
  (void)env;
  if (isCopy) *isCopy = 0;
  int i = find_fake_array(array);
  if (i >= 0 && g_fake_arrays[i].elem_size == 1)
    return (unsigned char *)g_fake_arrays[i].data;
  static unsigned char zero[1];
  return zero;
}

static void jni_ReleaseByteArrayElements(void *env, void *array,
                                         unsigned char *elems, jint mode) {
  (void)env; (void)array; (void)elems; (void)mode;
}

static void jni_GetByteArrayRegion(void *env, void *array, jint start,
                                   jint len, unsigned char *buf) {
  (void)env;
  int i = find_fake_array(array);
  if (i < 0 || g_fake_arrays[i].elem_size != 1 || !buf)
    return;
  unsigned char *d = (unsigned char *)g_fake_arrays[i].data;
  for (jint k = 0; k < len && (start + k) < g_fake_arrays[i].len; k++)
    buf[k] = d[start + k];
}

static void jni_SetByteArrayRegion(void *env, void *array, jint start,
                                   jint len, const unsigned char *buf) {
  (void)env;
  int i = find_fake_array(array);
  if (i < 0 || g_fake_arrays[i].elem_size != 1 || !buf)
    return;
  unsigned char *d = (unsigned char *)g_fake_arrays[i].data;
  for (jint k = 0; k < len && (start + k) < g_fake_arrays[i].len; k++)
    d[start + k] = buf[k];
}

static void jni_SetIntArrayRegion(void *env, void *array, jint start,
                                  jint len, const jint *buf) {
  (void)env;
  int i = find_fake_array(array);
  debugPrintf("jni_shim: SetIntArrayRegion(%p, %d, %d) idx=%d\n", array,
              (int)start, (int)len, i);
  if (i < 0 || g_fake_arrays[i].elem_size != (int)sizeof(jint) || !buf)
    return;
  jint *d = (jint *)g_fake_arrays[i].data;
  for (jint k = 0; k < len && (start + k) < g_fake_arrays[i].len; k++)
    d[start + k] = buf[k];
}

static jint jni_RegisterNatives(void *env, void *clazz, const void *methods,
                                jint nMethods) {
  (void)env; (void)clazz; (void)methods;
  debugPrintf("jni_shim: RegisterNatives(%d) -> 0\n", (int)nMethods);
  return 0;
}

static long long jni_CallLongMethod(void *env, void *obj, void *methodID, ...) {
  (void)env;
  log_call_id("CallLongMethod", methodID);
  if (methodID == &g_method_tags[MID_BUNDLE_GET_LONG]) {
    void *key_str = NULL;
    long long default_value = 0;
    if (g_static_args_override) {
      key_str = g_static_args[0];
      default_value = g_static_long_args[1];
    } else {
      va_list ap;
      va_start(ap, methodID);
      key_str = va_arg(ap, void *);
      if (strstr(id_sig(methodID), "Ljava/lang/String;J"))
        default_value = va_arg(ap, long long);
      va_end(ap);
    }
    long long value =
        bundle_get_long(obj, resolve_jstring(key_str), default_value);
    jni_trace("jni_shim:   -> Bundle.getLong(\"%s\") = %lld\n",
              resolve_jstring(key_str), value);
    return value;
  }
  return 0;
}

static long long jni_CallLongMethodV(void *env, void *obj, void *methodID,
                                     va_list args) {
  uintptr_t old_ret = g_jni_callsite_ret;
  g_jni_callsite_ret = (uintptr_t)__builtin_return_address(0);
  set_object_args_from_va(methodID, args);
  long long ret = jni_CallLongMethod(env, obj, methodID);
  clear_static_args_override();
  g_jni_callsite_ret = old_ret;
  return ret;
}

static long long jni_CallLongMethodA(void *env, void *obj, void *methodID,
                                     const jvalue *args) {
  uintptr_t old_ret = g_jni_callsite_ret;
  g_jni_callsite_ret = (uintptr_t)__builtin_return_address(0);
  set_object_args_from_jvalue(methodID, args);
  long long ret = jni_CallLongMethod(env, obj, methodID);
  clear_static_args_override();
  g_jni_callsite_ret = old_ret;
  return ret;
}

static long long jni_CallStaticLongMethod(void *env, void *clazz,
                                          void *methodID, ...) {
  (void)env; (void)clazz;
  LOG_STATIC_CALL("CallStaticLongMethod", methodID);
  if (methodID == &g_method_tags[MID_GET_MILLISECONDS]) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
  }
  return 0;
}

/* ---- JavaVM functions ---- */

static jint vm_DestroyJavaVM(void *vm) {
  (void)vm;
  return 0;
}

static jint vm_AttachCurrentThread(void *vm, void **penv, void *args) {
  (void)vm;
  (void)args;
  debugPrintf("jni_shim: AttachCurrentThread\n");
  if (penv)
    *penv = &jni_env_ptr;
  return 0;
}

static jint vm_DetachCurrentThread(void *vm) {
  (void)vm;
  return 0;
}

static jint vm_GetEnv(void *vm, void **penv, jint version) {
  (void)vm;
  (void)version;
  debugPrintf("jni_shim: GetEnv(version=0x%x)\n", version);
  if (penv)
    *penv = &jni_env_ptr;
  return 0;
}

static jint vm_AttachCurrentThreadAsDaemon(void *vm, void **penv, void *args) {
  (void)vm;
  (void)args;
  if (penv)
    *penv = &jni_env_ptr;
  return 0;
}

/* ---- Init ---- */

void jni_shim_init(void **out_vm, void **out_env) {
  for (int i = 0; i < JNI_VTABLE_SIZE; i++) {
    jni_env_vtable[i] = (uintptr_t)jni_stub;
    java_vm_vtable[i] = (uintptr_t)jni_stub;
  }

  /*
   * JNIEnv vtable indices from Android NDK jni.h.
   * C++ wrappers in the .so call the *V (va_list) variants,
   * so we must set both the variadic and V slots.
   *
   *   0-3:   reserved
   *   4:     GetVersion
   *   6:     FindClass
   *  15:     ExceptionOccurred
   *  17:     ExceptionClear
   *  21:     NewGlobalRef
   *  22:     DeleteGlobalRef
   *  23:     DeleteLocalRef
   *  25:     NewLocalRef
   *  31:     GetObjectClass
   *  33:     GetMethodID
   *  34/35:  CallObjectMethod / V
   *  37/38:  CallBooleanMethod / V
   *  49/50:  CallIntMethod / V
   *  61/62:  CallVoidMethod / V
   *  94:     GetFieldID
   * 113:     GetStaticMethodID
   * 114/115: CallStaticObjectMethod / V
   * 117/118: CallStaticBooleanMethod / V
   * 129/130: CallStaticIntMethod / V
   * 141/142: CallStaticVoidMethod / V
   * 144:     GetStaticFieldID
   * 145:     GetStaticObjectField
   * 150:     GetStaticIntField
   * 167:     NewStringUTF
   * 168:     GetStringUTFLength
   * 169:     GetStringUTFChars
   * 170:     ReleaseStringUTFChars
   * 171:     GetArrayLength
   * 205:     ExceptionCheck
   */
  jni_env_vtable[4] = (uintptr_t)jni_GetVersion;
  jni_env_vtable[6] = (uintptr_t)jni_FindClass;
  jni_env_vtable[15] = (uintptr_t)jni_ExceptionOccurred;
  jni_env_vtable[16] = (uintptr_t)jni_ExceptionDescribe;
  jni_env_vtable[17] = (uintptr_t)jni_ExceptionClear;
  jni_env_vtable[21] = (uintptr_t)jni_NewGlobalRef;
  jni_env_vtable[22] = (uintptr_t)jni_DeleteGlobalRef;
  jni_env_vtable[23] = (uintptr_t)jni_DeleteLocalRef;
  jni_env_vtable[25] = (uintptr_t)jni_NewLocalRef;
  jni_env_vtable[28] = (uintptr_t)jni_NewObject;
  jni_env_vtable[29] = (uintptr_t)jni_NewObject;           /* V variant */
  jni_env_vtable[30] = (uintptr_t)jni_NewObject;           /* A variant */
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
  jni_env_vtable[56] = (uintptr_t)jni_CallFloatMethod;     /* V */
  jni_env_vtable[57] = (uintptr_t)jni_CallFloatMethod;     /* A */
  jni_env_vtable[61] = (uintptr_t)jni_CallVoidMethod;
  jni_env_vtable[62] = (uintptr_t)jni_CallVoidMethodV;
  jni_env_vtable[63] = (uintptr_t)jni_CallVoidMethodA;
  jni_env_vtable[94] = (uintptr_t)jni_GetFieldID;
  jni_env_vtable[113] = (uintptr_t)jni_GetStaticMethodID;
  jni_env_vtable[114] = (uintptr_t)jni_CallStaticObjectMethod;
  jni_env_vtable[115] = (uintptr_t)jni_CallStaticObjectMethodV;
  jni_env_vtable[116] = (uintptr_t)jni_CallStaticObjectMethodA;
  jni_env_vtable[117] = (uintptr_t)jni_CallStaticBooleanMethod;
  jni_env_vtable[118] = (uintptr_t)jni_CallStaticBooleanMethodV;
  jni_env_vtable[119] = (uintptr_t)jni_CallStaticBooleanMethodA;
  jni_env_vtable[129] = (uintptr_t)jni_CallStaticIntMethod;
  jni_env_vtable[130] = (uintptr_t)jni_CallStaticIntMethod; /* V */
  jni_env_vtable[131] = (uintptr_t)jni_CallStaticIntMethod; /* A */
  jni_env_vtable[132] = (uintptr_t)jni_CallStaticLongMethod;
  jni_env_vtable[133] = (uintptr_t)jni_CallStaticLongMethod; /* V */
  jni_env_vtable[134] = (uintptr_t)jni_CallStaticLongMethod; /* A */
  jni_env_vtable[135] = (uintptr_t)jni_CallStaticFloatMethod;
  jni_env_vtable[136] = (uintptr_t)jni_CallStaticFloatMethod; /* V */
  jni_env_vtable[137] = (uintptr_t)jni_CallStaticFloatMethod; /* A */
  jni_env_vtable[141] = (uintptr_t)jni_CallStaticVoidMethod;
  jni_env_vtable[142] = (uintptr_t)jni_CallStaticVoidMethodV;
  jni_env_vtable[143] = (uintptr_t)jni_CallStaticVoidMethodA;
  jni_env_vtable[144] = (uintptr_t)jni_GetStaticFieldID;
  jni_env_vtable[145] = (uintptr_t)jni_GetStaticObjectField;
  jni_env_vtable[150] = (uintptr_t)jni_GetStaticIntField;
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
  jni_env_vtable[184] = (uintptr_t)jni_GetByteArrayElements;
  jni_env_vtable[187] = (uintptr_t)jni_GetIntArrayElements;     /* GetIntArrayElements */
  jni_env_vtable[192] = (uintptr_t)jni_ReleaseByteArrayElements;
  jni_env_vtable[195] = (uintptr_t)jni_ReleaseIntArrayElements; /* ReleaseIntArrayElements */
  jni_env_vtable[200] = (uintptr_t)jni_GetByteArrayRegion;
  jni_env_vtable[203] = (uintptr_t)jni_GetIntArrayRegion;       /* GetIntArrayRegion */
  jni_env_vtable[205] = (uintptr_t)jni_GetFloatArrayRegion;     /* GetFloatArrayRegion */
  jni_env_vtable[208] = (uintptr_t)jni_SetByteArrayRegion;
  jni_env_vtable[211] = (uintptr_t)jni_SetIntArrayRegion;
  jni_env_vtable[215] = (uintptr_t)jni_RegisterNatives;
  jni_env_vtable[219] = (uintptr_t)jni_GetJavaVM;                /* GetJavaVM */
  jni_env_vtable[228] = (uintptr_t)jni_ExceptionCheck;          /* ExceptionCheck (228 na spec) */

  jni_env_ptr = jni_env_vtable;

  /* JavaVM vtable */
  java_vm_vtable[3] = (uintptr_t)vm_DestroyJavaVM;
  java_vm_vtable[4] = (uintptr_t)vm_AttachCurrentThread;
  java_vm_vtable[5] = (uintptr_t)vm_DetachCurrentThread;
  java_vm_vtable[6] = (uintptr_t)vm_GetEnv;
  java_vm_vtable[7] = (uintptr_t)vm_AttachCurrentThreadAsDaemon;

  java_vm_ptr = java_vm_vtable;

  if (out_vm)
    *out_vm = &java_vm_ptr;
  if (out_env)
    *out_env = &jni_env_ptr;

  debugPrintf("jni_shim: Initialized (vm=%p, env=%p)\n", &java_vm_ptr,
              &jni_env_ptr);
}
