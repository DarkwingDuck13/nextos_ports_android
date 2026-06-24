#include "jni_shim.h"
#include "so_util.h"
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

static uintptr_t jni_env_vtable[JNI_VTABLE_SIZE];
static void *jni_env_ptr;

static uintptr_t java_vm_vtable[JNI_VTABLE_SIZE];
static void *java_vm_ptr;

enum {
  MID_UNKNOWN = 0,
  MID_ANDROID_HAS_VIBRATOR,
  MID_ANDROID_VIBRATE,
  MID_EXIT,
  MID_GET_VIEW_TOUCH_NUM,
  MID_GET_LOCAL_PATH,
  MID_PLAY_INTRO_VIDEO,
  MID_GENERIC,
};
static int g_method_tags[16];

static int g_dummy_array[4096];

#define MAX_JSTRINGS 64
static struct {
  void *handle;
  const char *value;
} g_jstrings[MAX_JSTRINGS];
static int g_jstring_count = 0;

static void *make_jstring(const char *value) {
  if (g_jstring_count >= MAX_JSTRINGS)
    g_jstring_count = 0;
  int idx = g_jstring_count++;
  g_jstrings[idx].handle = (void *)((uintptr_t)0x10000 + idx);
  g_jstrings[idx].value = value;
  return g_jstrings[idx].handle;
}

static const char *resolve_jstring(void *jstr) {
  for (int i = 0; i < g_jstring_count; i++) {
    if (g_jstrings[i].handle == jstr)
      return g_jstrings[i].value;
  }
  return "";
}

static intptr_t jni_stub(void) { return 0; }

static jint jni_GetVersion(void *env) {
  return 0x00010006; // JNI_VERSION_1_6
}

static void *jni_FindClass(void *env, const char *name) {
  debugPrintf("jni_shim: FindClass(%s)\n", name);
  static int fake_class;
  return &fake_class;
}

static void *jni_GetMethodID(void *env, void *clazz, const char *name,
                             const char *sig) {
  debugPrintf("jni_shim: GetMethodID(%s, %s)\n", name, sig);
  if (strcmp(name, "androidHasVibrator") == 0)
    return &g_method_tags[MID_ANDROID_HAS_VIBRATOR];
  if (strcmp(name, "androidVibrate") == 0)
    return &g_method_tags[MID_ANDROID_VIBRATE];
  if (strcmp(name, "exit") == 0)
    return &g_method_tags[MID_EXIT];
  if (strcmp(name, "getViewTouchNum") == 0)
    return &g_method_tags[MID_GET_VIEW_TOUCH_NUM];
  if (strcmp(name, "getLocalPath") == 0)
    return &g_method_tags[MID_GET_LOCAL_PATH];
  return &g_method_tags[MID_GENERIC];
}

static void *jni_GetStaticMethodID(void *env, void *clazz, const char *name,
                                   const char *sig) {
  debugPrintf("jni_shim: GetStaticMethodID(%s, %s)\n", name, sig);
  if (strcmp(name, "playIntroVideo") == 0)
    return &g_method_tags[MID_PLAY_INTRO_VIDEO];
  return &g_method_tags[MID_GENERIC];
}

static void *jni_CallObjectMethod(void *env, void *obj, void *methodID, ...) {
  debugPrintf("jni_shim: CallObjectMethod(mid=%p)\n", methodID);
  if (methodID == &g_method_tags[MID_GET_LOCAL_PATH]) {
    return make_jstring("./");
  }
  static int fake_obj;
  return &fake_obj;
}

static jboolean jni_CallBooleanMethod(void *env, void *obj, void *methodID,
                                      ...) {
  if (methodID == &g_method_tags[MID_ANDROID_HAS_VIBRATOR]) {
    return 0;
  }
  return 0;
}

static jint jni_CallIntMethod(void *env, void *obj, void *methodID, ...) {
  if (methodID == &g_method_tags[MID_GET_VIEW_TOUCH_NUM]) {
    return 0;
  }
  return 0;
}

static void jni_CallVoidMethod(void *env, void *obj, void *methodID, ...) {
  if (methodID == &g_method_tags[MID_EXIT]) {
    debugPrintf("jni_shim: Java Exit requested, exiting.\n");
    exit(0);
  }
}

static void *jni_CallStaticObjectMethod(void *env, void *clazz, void *methodID,
                                        ...) {
  static int fake_result;
  return &fake_result;
}

static void *unpause_thread_func(void *arg) {
  usleep(250000);
  debugPrintf("jni_shim: Intro video finished (simulated). Firing "
              "activeGame(true) callback to unpause!\n");
  void *env = arg;

  void (*f2f_activeGame)(void *, void *, jboolean, jint) =
      so_find_addr("Java_com_sega_f2fextension_F2FAndroidJNI_activeGame");
  if (f2f_activeGame) {
    for (int i = 1; i <= 255; i++) {
      f2f_activeGame(env, NULL, 1, i);
    }
  }

  void (*ct_activeGame)(void *, void *, jboolean, jint) =
      so_find_addr("Java_com_sega_CrazyTaxi_GL2JNILib_activeGame");
  if (ct_activeGame) {
    for (int i = 1; i <= 255; i++) {
      ct_activeGame(env, NULL, 1, i);
    }
  }

  debugPrintf("jni_shim: Unpause brute-force complete!\n");
  return NULL;
}

static void jni_CallStaticVoidMethod(void *env, void *clazz, void *methodID,
                                     ...) {
  if (methodID == &g_method_tags[MID_PLAY_INTRO_VIDEO]) {
    debugPrintf(
        "jni_shim: playIntroVideo called! Spawning unpause thread...\n");
    pthread_t thread;
    pthread_create(&thread, NULL, unpause_thread_func, env);
    pthread_detach(thread); // Let it clean up automatically
  }
}

static void jni_CallStaticVoidMethodV(void *env, void *clazz, void *methodID,
                                      va_list args) {
  jni_CallStaticVoidMethod(env, clazz, methodID);
}

static void jni_CallStaticVoidMethodA(void *env, void *clazz, void *methodID,
                                      const void *args) {
  jni_CallStaticVoidMethod(env, clazz, methodID);
}

static void *jni_NewStringUTF(void *env, const char *str) {
  return make_jstring(str ? str : "");
}

static jint jni_GetStringUTFLength(void *env, void *jstr) {
  return (jint)strlen(resolve_jstring(jstr));
}

static const char *jni_GetStringUTFChars(void *env, void *jstr, void *isCopy) {
  if (isCopy)
    *(unsigned char *)isCopy = 0; // JNI_FALSE
  return resolve_jstring(jstr);
}

static void jni_ReleaseStringUTFChars(void *env, void *jstr,
                                      const char *chars) {
  // No-op
}

static void *jni_NewGlobalRef(void *env, void *obj) { return obj; }
static void *jni_NewLocalRef(void *env, void *obj) { return obj; }
static void jni_DeleteGlobalRef(void *env, void *obj) {}
static void jni_DeleteLocalRef(void *env, void *obj) {}
static void *jni_GetObjectClass(void *env, void *obj) {
  static int f;
  return &f;
}

static jint jni_GetArrayLength(void *env, void *array) {
  (void)env;
  (void)array;
  return 0;
}

static void *jni_GetArrayElements(void *env, void *array, void *isCopy) {
  (void)env;
  (void)array;
  if (isCopy)
    *(unsigned char *)isCopy = 0;
  return g_dummy_array;
}

static void *jni_GetObjectArrayElement(void *env, void *array, jint index) {
  (void)env;
  (void)array;
  (void)index;
  return make_jstring(".");
}

static jboolean jni_ExceptionCheck(void *env) {
  (void)env;
  return 0;
}

static void jni_ExceptionClear(void *env) { (void)env; }

static void *jni_ExceptionOccurred(void *env) {
  (void)env;
  return NULL;
}

static jint vm_DestroyJavaVM(void *vm) { return 0; }
static jint vm_AttachCurrentThread(void *vm, void **penv, void *args) {
  if (penv)
    *penv = &jni_env_ptr;
  return 0;
}
static jint vm_DetachCurrentThread(void *vm) { return 0; }
static jint vm_GetEnv(void *vm, void **penv, jint version) {
  if (penv)
    *penv = &jni_env_ptr;
  return 0;
}

void *AAssetManager_fromJava(void *env, void *assetManager) {
  return (void *)0x1337;
}

void jni_shim_init(void **out_vm, void **out_env) {
  for (int i = 0; i < JNI_VTABLE_SIZE; i++) {
    jni_env_vtable[i] = (uintptr_t)jni_stub;
    java_vm_vtable[i] = (uintptr_t)jni_stub;
  }

  jni_env_vtable[4] = (uintptr_t)jni_GetVersion;
  jni_env_vtable[6] = (uintptr_t)jni_FindClass;
  jni_env_vtable[15] = (uintptr_t)jni_ExceptionOccurred;
  jni_env_vtable[17] = (uintptr_t)jni_ExceptionClear;
  jni_env_vtable[21] = (uintptr_t)jni_NewGlobalRef;
  jni_env_vtable[22] = (uintptr_t)jni_DeleteGlobalRef;
  jni_env_vtable[23] = (uintptr_t)jni_DeleteLocalRef;
  jni_env_vtable[25] = (uintptr_t)jni_NewLocalRef;
  jni_env_vtable[31] = (uintptr_t)jni_GetObjectClass;
  jni_env_vtable[33] = (uintptr_t)jni_GetMethodID;
  jni_env_vtable[34] = (uintptr_t)jni_CallObjectMethod;
  jni_env_vtable[35] = (uintptr_t)jni_CallObjectMethod;
  jni_env_vtable[37] = (uintptr_t)jni_CallBooleanMethod;
  jni_env_vtable[38] = (uintptr_t)jni_CallBooleanMethod;
  jni_env_vtable[49] = (uintptr_t)jni_CallIntMethod;
  jni_env_vtable[50] = (uintptr_t)jni_CallIntMethod;
  jni_env_vtable[61] = (uintptr_t)jni_CallVoidMethod;
  jni_env_vtable[62] = (uintptr_t)jni_CallVoidMethod;
  jni_env_vtable[113] = (uintptr_t)jni_GetStaticMethodID;
  jni_env_vtable[114] = (uintptr_t)jni_CallStaticObjectMethod;
  jni_env_vtable[115] = (uintptr_t)jni_CallStaticObjectMethod;

  jni_env_vtable[141] = (uintptr_t)jni_CallStaticVoidMethod;
  jni_env_vtable[142] = (uintptr_t)jni_CallStaticVoidMethodV;
  jni_env_vtable[143] = (uintptr_t)jni_CallStaticVoidMethodA;

  jni_env_vtable[167] = (uintptr_t)jni_NewStringUTF;
  jni_env_vtable[168] = (uintptr_t)jni_GetStringUTFLength;
  jni_env_vtable[169] = (uintptr_t)jni_GetStringUTFChars;
  jni_env_vtable[170] = (uintptr_t)jni_ReleaseStringUTFChars;
  jni_env_vtable[171] = (uintptr_t)jni_GetArrayLength;
  jni_env_vtable[173] = (uintptr_t)jni_GetObjectArrayElement;
  jni_env_vtable[205] = (uintptr_t)jni_ExceptionCheck;

  // Array Elements (Indices 183 to 190)
  jni_env_vtable[183] = (uintptr_t)jni_GetArrayElements; // Boolean
  jni_env_vtable[184] = (uintptr_t)jni_GetArrayElements; // Byte
  jni_env_vtable[185] = (uintptr_t)jni_GetArrayElements; // Char
  jni_env_vtable[186] = (uintptr_t)jni_GetArrayElements; // Short
  jni_env_vtable[187] = (uintptr_t)jni_GetArrayElements; // Int
  jni_env_vtable[188] = (uintptr_t)jni_GetArrayElements; // Long
  jni_env_vtable[189] = (uintptr_t)jni_GetArrayElements; // Float
  jni_env_vtable[190] = (uintptr_t)jni_GetArrayElements; // Double

  // JavaVM Vtable
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
}
