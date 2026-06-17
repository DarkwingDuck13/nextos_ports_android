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
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

#include "jni_shim.h"
#include "util.h"

#define JNI_VTABLE_SIZE 512

typedef int jint;

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
  MID_GENERIC,
  FID_OBB_VERSIONCODE,
  FID_GENERIC,
};

static int g_method_tags[16]; /* unique addresses used as method IDs */

/* ---- Configurable package/OBB ---- */
static const char *g_package_name = "com.microids.syberia";
static int g_obb_version = 12;

void jni_shim_set_package(const char *package_name, int obb_version) {
  g_package_name = package_name;
  g_obb_version = obb_version;
}

/* ---- Fake jstring tracking ---- */
/* We return tagged pointers as jstrings and map them to C strings */
#define MAX_JSTRINGS 1024
static struct {
  void *handle;
  char *value; /* copia propria (strdup) */
} g_jstrings[MAX_JSTRINGS];
static int g_jstring_count = 0;

/* jstring = o proprio ponteiro strdup (PERSISTENTE, unico, nunca liberado). O ring-buffer
   antigo (free + reuse de 1024 slots) LIBERAVA strings ainda em uso (ex: o path do PlayerPrefs
   guardado pelo Unity) -> apos >1024 jstrings, Unity usava ponteiro liberado -> crash em
   strchrnul/vsnprintf("%s_tmp", path_liberado). Identidade resolve isso (vaza, mas sessao limitada). */
static void *make_jstring(const char *value) {
  return (void *)strdup(value ? value : "");
}
static const char *resolve_jstring(void *jstr) {
  return jstr ? (const char *)jstr : "";
}
/* jnibridge proxy: dados no topo (usados cedo), funções definidas abaixo (precisam
   de jni_find_native). Ver bloco "EXECUTA Runnables postados". */
static struct { void *obj; long handle; } g_proxies[512];
static int g_proxy_n;
static int g_run_method_sentinel;   /* Method fake p/ Runnable.run() */
static int g_empty_args_sentinel;   /* Object[] vazio */
static int g_runnable_class_sentinel;
/* Choreographer frame-pacing (Unity 2021): o engine cria um Choreographer$FrameCallback
 * "UnityChoreographer" + HandlerThread/Looper e ESPERA o doFrame(frameTimeNanos) disparar
 * p/ avançar o frame. Nosso Looper é fake → doFrame nunca dispara → nativeRender do frame 2
 * trava. Capturamos o proxy do FrameCallback e uma driver-thread (main.c) chama doFrame
 * periodicamente (~60Hz) p/ destravar. */
static int g_doframe_method_sentinel;   /* Method fake p/ FrameCallback.doFrame(long) */
static int g_doframe_args_sentinel;     /* Object[1] = { Long(frameTimeNanos) } */
static int g_long_box_sentinel;         /* o Long boxed */
/* Handler$Callback.handleMessage(Message): a main posta uma Message via Handler.obtainMessage+
 * sendToTarget e ESPERA (cond nativo em libunity+0x2f3680) o Looper processá-la (handleMessage
 * → postFrameCallback). Looper fake nunca processa → deadlock. Dirigimos handleMessage no
 * sendToTarget. */
static int g_handlemsg_method_sentinel; /* Method fake p/ Handler$Callback.handleMessage(Message)Z */
static int g_handlemsg_args_sentinel;   /* Object[1] = { Message } */
static int g_message_sentinel;          /* a Message (obtainMessage->sendToTarget) */
static volatile int g_message_what;     /* msg.what passado ao obtainMessage */
void jni_handlemessage(void *env);
static _Thread_local int g_next_proxy_is_framecb;  /* setado por FindClass(Choreographer$FrameCallback) */
static void *volatile g_framecb_proxy;  /* proxy do FrameCallback capturado */
static volatile long g_doframe_nanos;   /* frameTimeNanos atual (a driver-thread atualiza) */
static void proxy_register(void *obj, long h);
static long proxy_handle(void *obj);
static void run_runnable(void *env, void *runnable);
int jni_is_run_method(void *o);
int jni_is_empty_args(void *o);

/* ---- SharedPreferences em memória (key->value) ----
 * O Cuphead salva cuphead_settings_data_v1 via putString e LÊ de volta via
 * getString/contains. Sem persistência, getString devolvia o default e contains=0
 * → o SaveManager re-tentava/livelock e/ou crashava em null. Aqui guardamos os
 * pares (strings e ints) numa tabela simples; o round-trip passa a funcionar. */
#define MAX_PREFS 128
static struct { char *key; char *sval; int ival; int has_s, has_i; } g_prefs[MAX_PREFS];
static int g_prefs_n = 0;
static int prefs_find(const char *key) {
  for (int i = 0; i < g_prefs_n; i++)
    if (g_prefs[i].key && !strcmp(g_prefs[i].key, key)) return i;
  return -1;
}
static int prefs_slot(const char *key) {
  int i = prefs_find(key);
  if (i >= 0) return i;
  if (g_prefs_n >= MAX_PREFS) return -1;
  g_prefs[g_prefs_n].key = strdup(key ? key : "");
  return g_prefs_n++;
}
static void prefs_put_string(const char *key, const char *val) {
  int i = prefs_slot(key); if (i < 0) return;
  if (g_prefs[i].sval) free(g_prefs[i].sval);
  g_prefs[i].sval = strdup(val ? val : ""); g_prefs[i].has_s = 1;
}
static void prefs_put_int(const char *key, int val) {
  int i = prefs_slot(key); if (i < 0) return;
  g_prefs[i].ival = val; g_prefs[i].has_i = 1;
}
static const char *prefs_get_string(const char *key) {
  int i = prefs_find(key);
  return (i >= 0 && g_prefs[i].has_s) ? g_prefs[i].sval : NULL;
}
static int prefs_contains(const char *key) {
  int i = prefs_find(key);
  return (i >= 0 && (g_prefs[i].has_s || g_prefs[i].has_i)) ? 1 : 0;
}

/* ---- Registry de method/field IDs por NOME (recon Unity) ---- */
struct mid_entry { const char *name; const char *sig; };
static struct mid_entry g_midreg[1024];
static int g_midreg_count = 0;

static void *reg_mid(const char *name, const char *sig) {
  for (int i = 0; i < g_midreg_count; i++)
    if (g_midreg[i].name == name ||
        (name && g_midreg[i].name && strcmp(g_midreg[i].name, name) == 0))
      return &g_midreg[i];
  if (g_midreg_count >= 1024) g_midreg_count = 0;
  int i = g_midreg_count++;
  g_midreg[i].name = name;
  g_midreg[i].sig = sig;
  return &g_midreg[i];
}
static const char *mid_name(void *tag) {
  if ((char *)tag >= (char *)g_midreg &&
      (char *)tag < (char *)(g_midreg + 1024))
    return ((struct mid_entry *)tag)->name;
  return NULL;
}

/* ===================================================================
 * AssetManager bridge — le de /storage/hollow-recon/assets/<path>
 * (Unity: getAssets() + AssetManager.open(path) + InputStream.read/close)
 * =================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#define ASSET_BASE "/storage/roms/terraria/"

static int g_assetmgr;   /* tag do objeto AssetManager */
static int g_empty_list; /* tag de uma java.util.List vazia */
static int g_iterator;   /* tag de um Iterator vazio */
static int g_appinfo;    /* tag de ApplicationInfo */

/* --- byte[] tracking (backing real) --- */
#define MAX_BARR 128
struct barr { unsigned char *buf; int len; };
static struct barr g_barr[MAX_BARR];
static int g_barr_n = 0;
static void *barr_new(int len) {
  int i = g_barr_n++ % MAX_BARR;
  if (g_barr[i].buf) free(g_barr[i].buf);
  g_barr[i].buf = (unsigned char *)malloc(len > 0 ? len : 1);
  g_barr[i].len = len;
  return &g_barr[i];
}
static struct barr *barr_find(void *h) {
  if ((char *)h >= (char *)g_barr && (char *)h < (char *)(g_barr + MAX_BARR))
    return (struct barr *)h;
  return NULL;
}
/* int[] real (p/ InputDevice.getDeviceIds): len = nº de ELEMENTOS, buf = 4*len bytes */
static void *iarr_new(const int *vals, int n) {
  int i = g_barr_n++ % MAX_BARR;
  if (g_barr[i].buf) free(g_barr[i].buf);
  g_barr[i].buf = (unsigned char *)malloc(n > 0 ? n * 4 : 4);
  g_barr[i].len = n;
  if (vals && n > 0) memcpy(g_barr[i].buf, vals, n * 4);
  else if (n > 0) memset(g_barr[i].buf, 0, n * 4);
  return &g_barr[i];
}

/* --- InputStream (FILE*) tracking --- */
#define MAX_ASTREAMS 32
struct astream { FILE *fp; long size; };
static struct astream g_astreams[MAX_ASTREAMS];
static int g_astream_n = 0;
static void *asset_open(const char *path) {
  char full[1200];
  /* alguns acessos vêm com prefixo "assets/" (ex il2cpp resource check do guid);
     nossos arquivos estão em ASSET_BASE/bin/Data (sem "assets/"). Tira o prefixo. */
  const char *p = path ? path : "";
  if (!strncmp(p, "assets/", 7)) p += 7;
  snprintf(full, sizeof(full), ASSET_BASE "%s", p);
  FILE *fp = fopen(full, "rb");
  debugPrintf("asset: open(%s) -> %s\n", path ? path : "?",
              fp ? "OK" : "FALHOU (sem arquivo)");
  if (!fp) return NULL;
  int i = g_astream_n++ % MAX_ASTREAMS;
  if (g_astreams[i].fp) fclose(g_astreams[i].fp);
  g_astreams[i].fp = fp;
  fseek(fp, 0, SEEK_END);
  g_astreams[i].size = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  return &g_astreams[i];
}
static struct astream *astream_find(void *h) {
  if ((char *)h >= (char *)g_astreams &&
      (char *)h < (char *)(g_astreams + MAX_ASTREAMS))
    return (struct astream *)h;
  return NULL;
}

/* --- JNI byte-array functions --- */
static void *jni_NewByteArray(void *env, int len) {
  (void)env;
  return barr_new(len);
}
static int jni_GetArrayLength_real(void *env, void *arr) {
  (void)env;
  struct barr *b = barr_find(arr);
  return b ? b->len : 0;
}
static void *jni_GetByteArrayElements(void *env, void *arr, void *isCopy) {
  (void)env;
  if (isCopy) *(unsigned char *)isCopy = 0;
  struct barr *b = barr_find(arr);
  return b ? b->buf : NULL;
}
static void jni_ReleaseByteArrayElements(void *env, void *arr, void *elems,
                                         int mode) {
  (void)env; (void)arr; (void)elems; (void)mode;
}
static void jni_GetByteArrayRegion(void *env, void *arr, int start, int len,
                                   void *buf) {
  (void)env;
  struct barr *b = barr_find(arr);
  if (b && start >= 0 && len >= 0 && start + len <= b->len)
    memcpy(buf, b->buf + start, len);
}
static void jni_SetByteArrayRegion(void *env, void *arr, int start, int len,
                                   const void *buf) {
  (void)env;
  struct barr *b = barr_find(arr);
  if (b && start >= 0 && len >= 0 && start + len <= b->len)
    memcpy(b->buf + start, buf, len);
}

/* ---- Generic stub ---- */
static intptr_t jni_stub(void) { return 0; }

/* ---- JNIEnv functions ---- */

static jint jni_GetVersion(void *env) {
  (void)env;
  return 0x00010006;
}

/* ===== Injeção de input p/ nativeInjectEvent (KeyEvent) =====
   nativeInjectEvent lê o evento via JNI (getAction/getKeyCode/...). Setamos
   g_hk_inject ANTES de chamar nativeInjectEvent e os métodos retornam daqui. */
struct hk_inject_s { int action, keycode, source, deviceId, metaState, repeat,
                     scancode, flags, unicode; long eventTime, downTime; };
struct hk_inject_s g_hk_inject;       /* exportado p/ main_recon */
static int g_obj_keyevent;            /* sentinela do objeto KeyEvent */
void *hk_keyevent_object(void) { return &g_obj_keyevent; }
static int g_gamepad_device;          /* sentinela do InputDevice (Xbox 360 virtual) */

/* classes distintas por nome (Unity compara KeyEvent.class vs MotionEvent.class) */
static struct { const char *name; int tag; } g_classreg[128];
static int g_classreg_n = 0;
int g_fmod_device_obj;   /* sentinela do org.fmod.FMODAudioDevice (NewObject/métodos do FMOD) */
static void *class_for(const char *name) {
  if (!name) name = "?";
  for (int i = 0; i < g_classreg_n; i++)
    if (g_classreg[i].name == name ||
        (g_classreg[i].name && strcmp(g_classreg[i].name, name) == 0))
      return &g_classreg[i].tag;
  if (g_classreg_n >= 128) g_classreg_n = 0;
  int i = g_classreg_n++;
  g_classreg[i].name = name;
  return &g_classreg[i].tag;
}
static void *jni_FindClass(void *env, const char *name) {
  (void)env;
  debugPrintf("jni_shim: FindClass(%s)\n", name);
  /* o proxy criado logo após FindClass(Choreographer$FrameCallback) é o FrameCallback */
  g_next_proxy_is_framecb = (name && strstr(name, "Choreographer$FrameCallback")) ? 1 : 0;
  return class_for(name);
}

static void *jni_GetMethodID(void *env, void *clazz, const char *name,
                             const char *sig) {
  (void)env;
  (void)clazz;
  debugPrintf("jni_shim: GetMethodID(%s, %s)\n", name, sig);
  return reg_mid(name, sig);
}

static void *jni_GetStaticMethodID(void *env, void *clazz, const char *name,
                                   const char *sig) {
  (void)env;
  (void)clazz;
  debugPrintf("jni_shim: GetStaticMethodID(%s, %s)\n", name, sig);
  return reg_mid(name, sig);
}

static void *jni_GetFieldID(void *env, void *clazz, const char *name,
                            const char *sig) {
  (void)env;
  (void)clazz;
  debugPrintf("jni_shim: GetFieldID(%s, %s)\n", name, sig);
  return reg_mid(name, sig);   /* registra por nome (DisplayMetrics fields) */
}

/* GetIntField (idx 100): DisplayMetrics widthPixels/heightPixels/densityDpi.
   0 aqui -> engine ve resolucao invalida -> "Unable to initialize Unity Engine". */
static jint jni_GetIntField(void *env, void *obj, void *fieldID) {
  (void)env; (void)obj;
  const char *nm = mid_name(fieldID);
  if (nm) {
    if (obj == (void *)&g_message_sentinel && strcmp(nm, "what") == 0) return g_message_what;
    if (strcmp(nm, "widthPixels") == 0) return 1280;
    if (strcmp(nm, "heightPixels") == 0) return 720;
    if (strcmp(nm, "densityDpi") == 0) return 160;
  }
  return 0;
}

/* GetFloatField (idx 102): DisplayMetrics density/xdpi/ydpi/scaledDensity.
   density/xdpi=0.0 -> divisão por zero / DPI inválido no engine -> loop de getMetrics. */
static float jni_GetFloatField(void *env, void *obj, void *fieldID) {
  (void)env; (void)obj;
  const char *nm = mid_name(fieldID);
  if (nm) {
    if (strcmp(nm, "density") == 0) return 1.0f;
    if (strcmp(nm, "scaledDensity") == 0) return 1.0f;
    if (strcmp(nm, "xdpi") == 0) return 160.0f;
    if (strcmp(nm, "ydpi") == 0) return 160.0f;
    if (strcmp(nm, "refreshRate") == 0) return 60.0f;
  }
  return 0.0f;
}

/* CallFloatMethodV (idx 56): Display.getRefreshRate() -> 60Hz (0 quebra o engine) */
static float jni_CallFloatMethodV(void *env, void *obj, void *methodID, va_list ap) {
  (void)env; (void)obj; (void)ap;
  const char *nm = mid_name(methodID);
  if (nm && strcmp(nm, "getRefreshRate") == 0) return 60.0f;
  return 0.0f;
}
static float jni_CallFloatMethod(void *env, void *obj, void *methodID, ...) {
  va_list ap; va_start(ap, methodID);
  float r = jni_CallFloatMethodV(env, obj, methodID, ap);
  va_end(ap);
  return r;
}

static void *jni_GetStaticFieldID(void *env, void *clazz, const char *name,
                                  const char *sig) {
  (void)env;
  (void)clazz;
  debugPrintf("jni_shim: GetStaticFieldID(%s, %s)\n", name, sig);
  if (strcmp(name, "OBB_VERSIONCODE") == 0)
    return &g_method_tags[FID_OBB_VERSIONCODE];
  /* registra o nome p/ GetStaticObjectField devolver a chave certa
     (AudioManager.PROPERTY_OUTPUT_*  -> getProperty distingue) */
  return reg_mid(name, sig);
}

/* CallObjectMethod — Unity (C++) usa a variante V (va_list); dispatch nela. */
static void *jni_CallObjectMethodV(void *env, void *obj, void *methodID,
                                   va_list ap) {
  (void)env;
  const char *nm = mid_name(methodID);
  debugPrintf("jni_shim: CallObjectMethod(%s)\n", nm ? nm : "?");
  static int fake_obj;
  if (nm) {
    if (strcmp(nm, "getPackageName") == 0)
      return make_jstring(g_package_name);
    /* ---- Gamepad Xbox 360 virtual (TER_GAMEPAD): InputManager.getInputDevice(id) + getters ---- */
    if (strcmp(nm, "getInputDevice") == 0) return &g_gamepad_device;
    if (obj == (void *)&g_gamepad_device) {
      if (strcmp(nm, "getName") == 0)          return make_jstring("Microsoft X-Box 360 pad");
      if (strcmp(nm, "getDescriptor") == 0)    return make_jstring("xbox360pad-virtual");
      if (strcmp(nm, "getMotionRanges") == 0)  return &g_empty_list;
      if (strcmp(nm, "getMotionRange") == 0)   return NULL;       /* sem range específico */
      if (strcmp(nm, "getVibrator") == 0)      return obj;        /* não-nulo */
      if (strcmp(nm, "getKeyCharacterMap") == 0) return obj;      /* não-nulo */
      return obj;   /* qualquer outro método do device -> não-nulo */
    }
    /* Play Asset Delivery: getAssetPackPath(name) -> dir REAL onde estão os arquivos
       do pack. Nossos dados estão em ASSET_BASE/bin/Data; Unity lê <path>/bin/Data/...
       então o path do pack é o ASSET_BASE (sem barra final). */
    if (strcmp(nm, "getAssetPackPath") == 0)
      return make_jstring("/storage/roms/terraria");
    /* anti-pirataria: jogo checa se foi instalado da Play Store. "" trava/loopa. */
    if (strcmp(nm, "getInstallerPackageName") == 0)
      return make_jstring("com.android.vending");
    /* Method fake do Runnable (jnibridge invoke): getName()->"run" p/ o C# despachar. */
    if (jni_is_run_method(obj)) {
      if (strcmp(nm, "getName") == 0) return make_jstring("run");
      return &g_run_method_sentinel; /* getReturnType/getParameterTypes/... -> não-nulo */
    }
    /* Method fake do FrameCallback (Choreographer): getName()->"doFrame". */
    if (obj == (void *)&g_doframe_method_sentinel) {
      if (strcmp(nm, "getName") == 0) return make_jstring("doFrame");
      return &g_doframe_method_sentinel;
    }
    /* Method fake do Handler$Callback: getName()->"handleMessage". */
    if (obj == (void *)&g_handlemsg_method_sentinel) {
      if (strcmp(nm, "getName") == 0) return make_jstring("handleMessage");
      return &g_handlemsg_method_sentinel;
    }
    /* Handler.obtainMessage(what[,...]) -> a nossa Message sentinel (guarda o what). */
    if (strcmp(nm, "obtainMessage") == 0) {
      g_message_what = va_arg(ap, int);
      debugPrintf("jni_shim: obtainMessage(what=%d) -> Message sentinel\n", g_message_what);
      return &g_message_sentinel;
    }
    /* Long boxed (arg do doFrame): longValue()/valueOf devolvem o frameTimeNanos. */
    if (obj == (void *)&g_long_box_sentinel) {
      return &g_long_box_sentinel;   /* getClass etc. -> não-nulo */
    }
    /* ClassLoader.findLibrary("il2cpp") -> path real do .so (ja' carregamos no F1,
       mas o UnityPlayer valida via findLibrary+System.load senao "Failed to load Il2CPP") */
    if (strcmp(nm, "findLibrary") == 0) {
      void *libname = va_arg(ap, void *);
      const char *ln = resolve_jstring(libname);
      debugPrintf("jni_shim: findLibrary(%s)\n", ln);
      if (ln && strstr(ln, "il2cpp"))
        return make_jstring(ASSET_BASE "libil2cpp.so");
      if (ln && strstr(ln, "main"))
        return make_jstring(ASSET_BASE "libmain.so");
      if (ln && strstr(ln, "unity"))
        return make_jstring(ASSET_BASE "libunity.so");
      return make_jstring("");
    }
    /* AudioManager.getProperty(key) -> valores válidos p/ o FMOD não configurar
       buffer/samplerate=0 (parseInt do nosso stub dava 0 -> mixer travava no boot) */
    if (strcmp(nm, "getProperty") == 0) {
      void *keyo = va_arg(ap, void *);
      const char *key = resolve_jstring(keyo);
      const char *val = getenv("TER_AUDIO_RATE") ? getenv("TER_AUDIO_RATE") : "44100";
      if (key && strstr(key, "FRAMES_PER_BUFFER")) val = "256";
      debugPrintf("jni_shim: getProperty(%s) -> %s\n", key ? key : "?", val);
      return make_jstring(val);
    }
    /* AssetManager bridge */
    if (strcmp(nm, "getAssets") == 0) return &g_assetmgr;
    /* listas vazias (queryIntentActivities, etc.) + iterator vazio */
    if (strcmp(nm, "queryIntentActivities") == 0 ||
        strcmp(nm, "queryBroadcastReceivers") == 0 ||
        strcmp(nm, "getSystemSharedLibraryNames") == 0)
      return &g_empty_list;
    if (strcmp(nm, "iterator") == 0) return &g_iterator;
    if (strcmp(nm, "getApplicationInfo") == 0) return &g_appinfo;
    if ((strcmp(nm, "open") == 0 || strcmp(nm, "openNonAsset") == 0) &&
        obj == &g_assetmgr) {
      void *pathstr = va_arg(ap, void *);
      return asset_open(resolve_jstring(pathstr)); /* NULL se nao existe */
    }
    /* builders Android (Intent.addFlags/setData/...) retornam o proprio obj */
    if (strcmp(nm, "addFlags") == 0 || strcmp(nm, "setFlags") == 0 ||
        strcmp(nm, "setData") == 0 || strcmp(nm, "setAction") == 0 ||
        strcmp(nm, "append") == 0)
      return obj;
    /* SharedPreferences.edit() -> editor (encadeável); retorna o proprio obj */
    if (strcmp(nm, "edit") == 0) return obj;
    /* SharedPreferences.Editor.putString(key,val) -> ARMAZENA + retorna editor
       (encadeamento putString(...).putString(...).apply()). */
    if (strcmp(nm, "putString") == 0) {
      void *keyo = va_arg(ap, void *), *valo = va_arg(ap, void *);
      const char *key = resolve_jstring(keyo), *val = resolve_jstring(valo);
      prefs_put_string(key, val);
      debugPrintf("[PREFS] putString key='%s' (%zu bytes) ARMAZENADO\n", key, strlen(val));
      return obj;
    }
    if (strcmp(nm, "putInt") == 0) {
      void *keyo = va_arg(ap, void *); int val = va_arg(ap, int);
      prefs_put_int(resolve_jstring(keyo), val);
      debugPrintf("[PREFS] putInt key='%s' val=%d ARMAZENADO\n", resolve_jstring(keyo), val);
      return obj;
    }
    if (strcmp(nm, "putBoolean") == 0 || strcmp(nm, "putFloat") == 0 ||
        strcmp(nm, "putLong") == 0) return obj;  /* encadeamento */
    if (strcmp(nm, "remove") == 0) return obj;
    /* diretorios de dados -> path REAL gravavel (persistentDataPath do Unity).
       Sem isso (=""), PlayerPrefs/save quebram -> jogo trava em "first run". */
    if (strcmp(nm, "getFilesDir") == 0 || strcmp(nm, "getExternalFilesDir") == 0 ||
        strcmp(nm, "getCacheDir") == 0 || strcmp(nm, "getExternalCacheDir") == 0 ||
        strcmp(nm, "getDataDir") == 0 || strcmp(nm, "getExternalStorageDirectory") == 0 ||
        strcmp(nm, "getPath") == 0 || strcmp(nm, "getAbsolutePath") == 0 ||
        strcmp(nm, "getCanonicalPath") == 0)
      return make_jstring("/storage/roms/terraria/userdata");
    /* SharedPreferences.getString(key, default) -> valor ARMAZENADO se existir,
       senão o default. Faz o round-trip do save funcionar (era sempre default). */
    if (strcmp(nm, "getString") == 0) {
      void *keystr = va_arg(ap, void *);
      void *defstr = va_arg(ap, void *);
      const char *key = resolve_jstring(keystr);
      /* CUP_NOFX: força o jogo a CARREGAR settings com PÓS-PROCESSAMENTO OFF
         (chromaticAberration/noise/blur) — esses efeitos usam FBO/render-to-texture
         que TRAVAM o GPU Mali Utgard no carregamento do título. */
      if (getenv("CUP_NOFX") && key && strstr(key, "settings_data")) {
        static const char *FX_OFF =
          "{\"hasBootedUpGame\":true,\"overscan\":0.0,\"chromaticAberration\":0.0,"
          "\"screenWidth\":1280,\"screenHeight\":720,\"effects\":false,\"blur\":false,"
          "\"forceOriginalTitleScreen\":false,\"masterVolume\":0.0,\"sFXVolume\":0.0,"
          "\"musicVolume\":0.0,\"canVibrate\":true,\"rotateControlsWithCamera\":false,"
          "\"language\":-1,\"chromaticAberrationEffect\":false,\"noiseEffect\":false,"
          "\"subtleBlurEffect\":false,\"brightness\":0.0}";
        debugPrintf("[NOFX] getString settings -> efeitos OFF (anti-wedge Utgard)\n");
        return make_jstring(FX_OFF);
      }
      const char *stored = prefs_get_string(key);
      debugPrintf("[PREFS] getString key='%s' -> %s\n", key, stored ? "ARMAZENADO" : "default");
      if (stored) return make_jstring(stored);
      return defstr ? defstr : make_jstring("");
    }
    if (strcmp(nm, "toString") == 0)
      return make_jstring("");
    /* 🔑 TER_KBFIX: Class.getName()/getCanonicalName() na reflection de campos (Unity
       _AndroidJNIHelper.GetFieldID c/ sig vazio reflete field.getType().getName() p/ montar
       a assinatura). Sem isso o getName devolvia &fake_obj (não-string) → GetStringUTFChars=""
       → sig vazio → "Field X or type signature not found" → exceção em KeyboardInput.Update
       ABORTA o ExecuteFrame ANTES do Draw → tela preta. Devolver um nome de tipo válido faz a
       reflection montar uma assinatura e o GetFieldID/leitura seguir (campo lido = fake/0). */
    if (getenv("TER_KBFIX") &&
        (strcmp(nm, "getName") == 0 || strcmp(nm, "getCanonicalName") == 0 ||
         strcmp(nm, "getTypeName") == 0)) {
      static int gn = 0; if (gn++ < 30) { debugPrintf("[KBREFLECT] %s -> java.lang.Object\n", nm); }
      return make_jstring("java.lang.Object");
    }
    /* log de métodos de reflection p/ diagnóstico (gated) */
    if (getenv("TER_REFLOG") &&
        (strstr(nm,"Field")||strstr(nm,"Type")||strstr(nm,"Component")||
         strstr(nm,"getClass")||strstr(nm,"getDeclar")||strcmp(nm,"getType")==0)) {
      static int rn=0; if (rn++<40) debugPrintf("[REFLOG-obj] %s\n", nm);
    }
  }
  return &fake_obj;
}
static void *jni_CallObjectMethod(void *env, void *obj, void *methodID, ...) {
  va_list ap; va_start(ap, methodID);
  void *r = jni_CallObjectMethodV(env, obj, methodID, ap);
  va_end(ap);
  return r;
}

/* Setado por NewStringUTF("gles-api-check") (logo antes do getBoolean da pref);
 * consumido pelo próximo getBoolean -> retorna true ("aviso já dispensado") e o
 * jogo PULA o AlertDialog "hardware requirements" (que travava o boot, stub JNI).
 * Robusto contra o bug do CallBooleanMethodV (args via va_list, não parseáveis aqui). */
static volatile int g_gles_warn_skip;
static volatile int g_internet_deny_arm;  /* ver NewStringUTF/CallIntMethod (INTERNET denied) */

/* CallBooleanMethod V (index 38) — lê args via va_list (variante que il2cpp usa) */
static unsigned char jni_CallBooleanMethodV(void *env, void *obj,
                                            void *methodID, va_list ap) {
  (void)obj;
  const char *nm = mid_name(methodID);
  if (nm) {
    if (getenv("TER_REFLOG") && (strstr(nm,"isArray")||strstr(nm,"isPrimitive")||strstr(nm,"isAssign"))) {
      static int bn=0; if (bn++<40) debugPrintf("[REFLOG-bool] %s -> 0\n", nm);
    }
    if (strcmp(nm, "isEmpty") == 0) return 1;  /* lista vazia */
    if (strcmp(nm, "hasNext") == 0) return 0;  /* iterator vazio */
    /* Handler.post/postDelayed(Runnable[,delay]) -> RODA o Runnable, retorna true.
       (init deferida do Unity usa Handler.post; sem rodar, o boot trava no poll.) */
    if (strcmp(nm, "post") == 0 || strcmp(nm, "postDelayed") == 0 ||
        strcmp(nm, "postAtTime") == 0 || strcmp(nm, "postAtFrontOfQueue") == 0) {
      void *r = va_arg(ap, void *);
      if (!getenv("CUP_NORUNUI")) run_runnable(env, r);
      return 1;
    }
    /* SharedPreferences.contains(key) -> 1 se ARMAZENADO (round-trip do save). */
    if (strcmp(nm, "contains") == 0) {
      void *keyo = va_arg(ap, void *);
      const char *key = resolve_jstring(keyo);
      if (getenv("CUP_NOFX") && key && strstr(key, "settings_data")) return 1;
      int has = getenv("CUP_NOCONTAINS") ? 0 : prefs_contains(key);
      debugPrintf("[PREFS] contains key='%s' -> %d\n", key, has);
      return (unsigned char)has;
    }
    if (strcmp(nm, "commit") == 0) return 1;  /* Editor.commit() -> true */
    /* getBoolean: flag do NewStringUTF("gles-api-check") pula o AlertDialog GLES. */
    if (strcmp(nm, "getBoolean") == 0) {
      int v = 0;
      if (g_gles_warn_skip && !getenv("CUP_SHOWGLESWARN")) { v = 1; g_gles_warn_skip = 0; }
      debugPrintf("[PREFS] getBoolean -> %d (gles_skip flag)\n", v);
      return (unsigned char)v;
    }
  }
  return 0;
}
static unsigned char jni_CallBooleanMethod(void *env, void *obj, void *methodID, ...) {
  va_list ap; va_start(ap, methodID);
  unsigned char r = jni_CallBooleanMethodV(env, obj, methodID, ap);
  va_end(ap);
  return r;
}

/* CallIntMethod — variante V */
static jint jni_CallIntMethodV(void *env, void *obj, void *methodID,
                               va_list ap) {
  (void)env;
  const char *nm = mid_name(methodID);
  if (obj == (void *)&g_message_sentinel && nm && strcmp(nm, "getWhat") == 0)
    return g_message_what;
  /* org.fmod.FMODAudioDevice — qualquer método int/bool (start/isRunning/init...) = sucesso */
  if (obj == &g_fmod_device_obj) { debugPrintf("jni_shim: FMODAudioDevice.%s -> 1\n", nm?nm:"?"); return 1; }
  if (nm) {
    /* checkPermission(INTERNET): armado pelo NewStringUTF → DENIED(-1) p/ desligar a
       Unity Analytics (pula advertising-id/session-start que travava o boot). */
    if (g_internet_deny_arm &&
        (strcmp(nm, "checkCallingOrSelfPermission") == 0 ||
         strcmp(nm, "checkSelfPermission") == 0 ||
         strcmp(nm, "checkPermission") == 0)) {
      g_internet_deny_arm = 0;
      debugPrintf("jni_shim: %s(INTERNET) -> -1 (DENIED, analytics off)\n", nm);
      return -1;
    }
    /* android.os.StatFs — variantes int (API antiga). ~50GB livres p/ não bloquear save. */
    if (strcmp(nm, "getBlockSize") == 0) return 4096;
    if (strcmp(nm, "getAvailableBlocks") == 0 || strcmp(nm, "getFreeBlocks") == 0) return 13107200; /* ×4096=50GB */
    if (strcmp(nm, "getBlockCount") == 0) return 26214400; /* ×4096=100GB */
    /* ---- KeyEvent (nativeInjectEvent) ---- */
    /* ---- InputDevice Xbox 360 virtual (getters int) ---- */
    if (obj == (void *)&g_gamepad_device) {
      if (strcmp(nm, "getVendorId") == 0)        return 1118;       /* 0x045E Microsoft */
      if (strcmp(nm, "getProductId") == 0)       return 654;        /* 0x028E Xbox360 pad */
      if (strcmp(nm, "getSources") == 0)         return 0x1000611;  /* GAMEPAD|JOYSTICK|DPAD */
      if (strcmp(nm, "getId") == 0)              return 1;
      if (strcmp(nm, "getControllerNumber") == 0) return 1;
      if (strcmp(nm, "getKeyboardType") == 0)    return 0;
      if (strcmp(nm, "supportsSource") == 0)     return 1;
      return 0;
    }
    if (strcmp(nm, "getAction") == 0) { debugPrintf("[KEYEV] getAction->%d\n", g_hk_inject.action); return g_hk_inject.action; }
    if (strcmp(nm, "getKeyCode") == 0) { debugPrintf("[KEYEV] getKeyCode->%d\n", g_hk_inject.keycode); return g_hk_inject.keycode; }
    if (strcmp(nm, "getSource") == 0) return g_hk_inject.source;
    if (strcmp(nm, "getDeviceId") == 0) return g_hk_inject.deviceId;
    if (strcmp(nm, "getMetaState") == 0) return g_hk_inject.metaState;
    if (strcmp(nm, "getRepeatCount") == 0) return g_hk_inject.repeat;
    if (strcmp(nm, "getScanCode") == 0) return g_hk_inject.scancode;
    if (strcmp(nm, "getInt") == 0) { void *k = va_arg(ap, void *); int d = va_arg(ap, int);
      const char *key = resolve_jstring(k); int i = prefs_find(key);
      int v = (i >= 0 && g_prefs[i].has_i) ? g_prefs[i].ival : d;
      debugPrintf("[PREFS] getInt key='%s' def=%d -> %d\n", key, d, v); return v; }
    if (strcmp(nm, "getFlags") == 0) return g_hk_inject.flags;
    if (strcmp(nm, "getUnicodeChar") == 0) return g_hk_inject.unicode;
    if (strcmp(nm, "size") == 0) return 0; /* List/Collection vazia */
    /* ---- Display: o engine pega a resolucao/rotacao; 0x0 -> "Unable to
       initialize the Unity Engine". Devolve 1280x720, rotacao 0, displayId 0. ---- */
    if (strcmp(nm, "getWidth") == 0 || strcmp(nm, "getRawWidth") == 0) return 1280;
    if (strcmp(nm, "getHeight") == 0 || strcmp(nm, "getRawHeight") == 0) return 720;
    if (strcmp(nm, "getRotation") == 0) return 0;
    if (strcmp(nm, "getDisplayId") == 0) return 0;
  }
  struct astream *s = astream_find(obj);
  if (s && nm) {
    if (strcmp(nm, "read") == 0) {
      void *barr = va_arg(ap, void *);
      int off = va_arg(ap, int);
      int len = va_arg(ap, int);
      struct barr *b = barr_find(barr);
      if (!b) return -1;
      if (off < 0) off = 0;
      if (off + len > b->len) len = b->len - off;
      if (len <= 0) return -1;
      size_t n = fread(b->buf + off, 1, (size_t)len, s->fp);
      return n > 0 ? (int)n : -1; /* -1 = EOF */
    }
    if (strcmp(nm, "available") == 0) {
      long pos = ftell(s->fp);
      return (int)(s->size - pos);
    }
  }
  return 0;
}
static jint jni_CallIntMethod(void *env, void *obj, void *methodID, ...) {
  va_list ap; va_start(ap, methodID);
  jint r = jni_CallIntMethodV(env, obj, methodID, ap);
  va_end(ap);
  return r;
}

/* CallVoidMethod (index 94) */
static void jni_CallVoidMethodV(void *env, void *obj, void *methodID, va_list ap) {
  const char *nm = mid_name(methodID);
  debugPrintf("jni_shim: CallVoidMethod(%s)\n", nm ? nm : "?");
  /* Message.sendToTarget(): a main posta a Message no Looper (fake) e bloqueia num cond
     nativo esperando o handleMessage processá-la. Dirigimos handleMessage AQUI (síncrono no
     thread da main, ANTES dela bloquear → predicado já satisfeito). TER_NOHANDLEMSG desliga. */
  if (nm && strcmp(nm, "sendToTarget") == 0 && obj == (void *)&g_message_sentinel) {
    if (!getenv("TER_NOHANDLEMSG")) jni_handlemessage(env);
    return;
  }
  /* runOnUiThread/post(Runnable): EXECUTA o Runnable (senão Unity Analytics/init trava). */
  if (nm && (strcmp(nm, "runOnUiThread") == 0 || strcmp(nm, "post") == 0 ||
             strcmp(nm, "postAtFrontOfQueue") == 0)) {
    void *r = va_arg(ap, void *);
    /* roda o Runnable via invoke do jnibridge (handle lido pela variante V correta).
       CUP_NORUNUI desliga. */
    if (!getenv("CUP_NORUNUI")) run_runnable(env, r);
    return;
  }
  /* Play Asset Delivery: getAssetPackState(name, cb) — Unity espera o callback nativo
     nativeStatusQueryResult(name, status, errorCode). Respondemos COMPLETED(4) na hora:
     os packs (UnityDataAssetPack/StreamingAssets) já estão "instalados" em bin/Data. */
  if (nm && strcmp(nm, "getAssetPackState") == 0) {
    void *name_j = va_arg(ap, void *);
    const char *pn = resolve_jstring(name_j);
    void *fn = jni_find_native("nativeStatusQueryResult");
    debugPrintf("jni_shim: getAssetPackState(%s) -> COMPLETED via nativeStatusQueryResult=%p\n",
                pn ? pn : "?", fn);
    if (fn) {
      static int fake_clazz;
      /* (JNIEnv*, jclass, jstring name, jint status=4 COMPLETED, jint errorCode=0) */
      ((void (*)(void *, void *, void *, int, int))fn)(env, &fake_clazz, name_j, 4, 0);
    }
    return;
  }
  struct astream *s = astream_find(obj);
  if (s && nm && strcmp(nm, "close") == 0) {
    if (s->fp) { fclose(s->fp); s->fp = NULL; }
  }
}
static void jni_CallVoidMethod(void *env, void *obj, void *methodID, ...) {
  va_list ap; va_start(ap, methodID);
  jni_CallVoidMethodV(env, obj, methodID, ap);
  va_end(ap);
}

/* CallStaticObjectMethod (index 113) */
static void *jni_CallStaticObjectMethodV(void *env, void *clazz,
                                         void *methodID, va_list ap) {
  (void)env;
  (void)clazz;
  const char *nm = mid_name(methodID);
  /* InputDevice.getDeviceIds() -> int[] REAL (sem args). TER_GAMEPAD: 1 device (id=1) =
     o Xbox 360 virtual (o InControl tem profile pronto p/ Xbox → mapeamento correto). */
  if (nm && !strcmp(nm, "getDeviceIds")) {
    int ndev = getenv("CUP_NDEV") ? atoi(getenv("CUP_NDEV")) : 0;
    int ids[8]; for (int i = 0; i < ndev && i < 8; i++) ids[i] = 100 + i;
    if (getenv("TER_GAMEPAD")) { int one[1] = {1}; static int o2=0; if(!o2){o2=1;debugPrintf("getDeviceIds()->[1] (Xbox virtual)\n");} return iarr_new(one, 1); }
    static int once = 0; if (!once) { once = 1;
      debugPrintf("jni_shim: getDeviceIds() -> int[%d]\n", ndev); }
    return iarr_new(ids, ndev);
  }
  /* Environment.getExternalStorageState() -> "mounted" (senão o jogo acha o storage indisponível
     e mostra "low on storage" ao entrar no Single Player). */
  if (nm && !strcmp(nm, "getExternalStorageState")) return make_jstring("mounted");
  /* encode/decode (SaveManager): IDENTIDADE — devolve a própria string de entrada. */
  if (nm && (!strcmp(nm, "encode") || !strcmp(nm, "decode"))) {
    void *arg0 = va_arg(ap, void *);
    return arg0;
  }
  /* InputDevice.getDevice(id) é ESTÁTICO → o device Xbox 360 virtual (TER_GAMEPAD). */
  if (nm && !strcmp(nm, "getDevice")) {
    debugPrintf("jni_shim: getDevice() -> Xbox virtual\n");
    return &g_gamepad_device;
  }
  /* jnibridge: newInterfaceProxy(long handle, Class[] ifaces) -> proxy. Guarda o
     handle p/ rodar o Runnable depois (runOnUiThread). */
  if (nm && !strcmp(nm, "newInterfaceProxy")) {
    long h = va_arg(ap, long);
    void *proxy = malloc(16);
    proxy_register(proxy, h);
    debugPrintf("jni_shim: newInterfaceProxy(handle=%ld) -> %p\n", h, proxy);
    if (g_next_proxy_is_framecb) {
      g_framecb_proxy = proxy;
      g_next_proxy_is_framecb = 0;
      debugPrintf("jni_shim: [CHOREO] FrameCallback capturado: proxy=%p handle=%ld\n", proxy, h);
    }
    return proxy;
  }
  debugPrintf("jni_shim: CallStaticObjectMethod(%s)\n", nm ? nm : "?");
  static int fake_result;
  return &fake_result;  /* fake Class/objeto nao-nulo (forName etc.) */
}
static void *jni_CallStaticObjectMethod(void *env, void *clazz, void *methodID, ...) {
  va_list ap; va_start(ap, methodID);
  void *r = jni_CallStaticObjectMethodV(env, clazz, methodID, ap);
  va_end(ap);
  return r;
}

/* CallStaticBooleanMethod (index 124) */
static unsigned char jni_CallStaticBooleanMethod(void *env, void *clazz,
                                                 void *methodID, ...) {
  (void)env;
  (void)clazz;
  debugPrintf("jni_shim: CallStaticBooleanMethod(mid=%p) -> 1\n", methodID);
  // Return true for hasTouchScreen — prevents game from managing
  // Shield gamepad button layouts that don't exist in the OBB.
  return 1;
}

/* CallStaticIntMethod (index 136) */
static jint cint_dispatch(void *methodID, void *arg0) {
  const char *nm = mid_name(methodID);
  if (nm && (strcmp(nm, "parseInt") == 0 || strcmp(nm, "valueOf") == 0 ||
             strcmp(nm, "intValue") == 0)) {
    const char *str = resolve_jstring(arg0);
    int v = str ? atoi(str) : 0;
    debugPrintf("jni_shim: %s(%s) -> %d\n", nm, str ? str : "?", v);
    return v;
  }
  return 0;
}
static jint jni_CallStaticIntMethod(void *env, void *clazz, void *methodID, ...) {
  (void)env; (void)clazz;
  va_list ap; va_start(ap, methodID); void *a = va_arg(ap, void *); va_end(ap);
  return cint_dispatch(methodID, a);
}
static jint jni_CallStaticIntMethodV(void *env, void *clazz, void *methodID, va_list ap) {
  (void)env; (void)clazz;
  void *a = va_arg(ap, void *);
  return cint_dispatch(methodID, a);
}

/* CallStaticVoidMethod (index 145) */
static void jni_CallStaticVoidMethod(void *env, void *clazz, void *methodID,
                                     ...) {
  (void)env;
  (void)clazz;
  debugPrintf("jni_shim: CallStaticVoidMethod(mid=%p)\n", methodID);
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
  /* Build.VERSION.SDK_INT: 0 faz Unity 2021.3 abortar ("Unable to initialize the
     Unity Engine" — feature-level inválido). 30 = Android 11. */
  { const char *nm = mid_name(fieldID);
    if (nm && strcmp(nm, "SDK_INT") == 0) {
      debugPrintf("jni_shim: GetStaticIntField(SDK_INT) -> 30\n");
      return 30;
    } }
  debugPrintf("jni_shim: GetStaticIntField(fid=%p) -> 0\n", fieldID);
  return 0;
}

/* GetStaticObjectField (index 156) */
static void *jni_GetStaticObjectField(void *env, void *clazz, void *fieldID) {
  (void)env;
  (void)clazz;
  const char *nm = mid_name(fieldID);
  /* constantes String do AudioManager: devolver o NOME como valor p/ getProperty
     distinguir SAMPLE_RATE x FRAMES_PER_BUFFER */
  if (nm && (strstr(nm, "PROPERTY_") || strstr(nm, "SERVICE"))) {
    debugPrintf("jni_shim: GetStaticObjectField(%s) -> chave\n", nm);
    return make_jstring(nm);
  }
  /* android.os.Build.* — o crash-reporter/analytics coleta esses; devolver "" (nosso
     &fake antigo dava GetStringUTFChars=="") pode travar a init. Valores plausíveis. */
  if (nm) {
    if (!strcmp(nm, "MODEL")) return make_jstring("NextOS");
    if (!strcmp(nm, "DEVICE")) return make_jstring("Amlogic-no");
    if (!strcmp(nm, "MANUFACTURER")) return make_jstring("Amlogic");
    if (!strcmp(nm, "BRAND")) return make_jstring("NextOS");
    if (!strcmp(nm, "PRODUCT")) return make_jstring("X5M");
    if (!strcmp(nm, "HARDWARE")) return make_jstring("amlogic");
    if (!strcmp(nm, "BOARD")) return make_jstring("amlogic");
    if (!strcmp(nm, "FINGERPRINT")) return make_jstring("NextOS/X5M/Amlogic-no:9/PQ/1:user/release-keys");
    if (!strcmp(nm, "RELEASE")) return make_jstring("9");
    if (!strcmp(nm, "ID")) return make_jstring("PQ3A.190801.002");
    if (!strcmp(nm, "INCREMENTAL")) return make_jstring("1");
    if (!strcmp(nm, "TAGS")) return make_jstring("release-keys");
    if (!strcmp(nm, "TYPE")) return make_jstring("user");
    if (!strcmp(nm, "HOST")) return make_jstring("nextos");
    if (!strcmp(nm, "USER")) return make_jstring("nextos");
    if (!strcmp(nm, "SERIAL")) return make_jstring("unknown");
    if (!strcmp(nm, "DISPLAY")) return make_jstring("PQ");
    if (!strcmp(nm, "BOOTLOADER")) return make_jstring("unknown");
    if (!strcmp(nm, "CODENAME")) return make_jstring("REL");
  }
  debugPrintf("jni_shim: GetStaticObjectField(%s) -> fake\n", nm ? nm : "?");
  static int fake;
  return &fake;
}

/* NewStringUTF (index 167) */
static void *jni_NewStringUTF(void *env, const char *str) {
  (void)env;
  debugPrintf("jni_shim: NewStringUTF(%s)\n", str ? str : "(null)");
  /* arma o skip do aviso GLES p/ o próximo getBoolean (ver g_gles_warn_skip) */
  if (str && strstr(str, "gles-api-check")) g_gles_warn_skip = 1;
  /* arma INTERNET=DENIED p/ o próximo checkPermission → Unity Analytics pula o fluxo
     de rede/advertising-id (que postava um runOnUiThread Runnable e travava o boot).
     CUP_INETOK reabilita (= granted). */
  if (str && strstr(str, "permission.INTERNET") && !getenv("CUP_INETOK"))
    g_internet_deny_arm = 1;
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
  if (obj == &g_obj_keyevent) return class_for("android/view/KeyEvent");
  static int fake_obj_class;
  return &fake_obj_class;
}
static unsigned char jni_IsInstanceOf(void *env, void *obj, void *clazz) {
  (void)env;
  if (obj == &g_obj_keyevent) return clazz == class_for("android/view/KeyEvent");
  return 1; /* permissivo p/ outros casts */
}
static unsigned char jni_IsSameObject(void *env, void *a, void *b) {
  (void)env; return a == b;
}
/* CallLongMethod V — getEventTime/getDownTime do KeyEvent (retornam long) */
static long jni_CallLongMethodV(void *env, void *obj, void *methodID, va_list ap) {
  (void)env; (void)ap;
  if (obj == (void *)&g_long_box_sentinel) return g_doframe_nanos;  /* Long.longValue() do doFrame */
  const char *nm = mid_name(methodID);
  if (nm) {
    if (strcmp(nm, "getEventTime") == 0) return g_hk_inject.eventTime;
    if (strcmp(nm, "getDownTime") == 0) return g_hk_inject.downTime;
    if (strcmp(nm, "longValue") == 0) return g_doframe_nanos;
    /* android.os.StatFs — checagem de espaço livre (Single Player checa antes de salvar).
       Sem isso retorna 0 → "Your device is low on storage". Reportamos ~50GB livres. */
    if (strcmp(nm, "getAvailableBytes") == 0 || strcmp(nm, "getFreeBytes") == 0) return 50LL*1024*1024*1024;
    if (strcmp(nm, "getTotalBytes") == 0) return 100LL*1024*1024*1024;
    /* java.io.File.getUsableSpace()/getFreeSpace()/getTotalSpace() — outra via de checagem */
    if (strcmp(nm, "getUsableSpace") == 0 || strcmp(nm, "getFreeSpace") == 0) return 50LL*1024*1024*1024;
    if (strcmp(nm, "getTotalSpace") == 0) return 100LL*1024*1024*1024;
    if (strcmp(nm, "getBlockSizeLong") == 0) return 4096;
    if (strcmp(nm, "getAvailableBlocksLong") == 0 || strcmp(nm, "getBlockCountLong") == 0 || strcmp(nm, "getFreeBlocksLong") == 0) return 13107200LL; /* ×4096 = 50GB */
  }
  return 0;
}
static long jni_CallLongMethod(void *env, void *obj, void *methodID, ...) {
  va_list ap; va_start(ap, methodID);
  long r = jni_CallLongMethodV(env, obj, methodID, ap);
  va_end(ap);
  return r;
}

/* Exception handling */
static unsigned char jni_ExceptionCheck(void *env) {
  (void)env;
  return 0;
}
static void jni_ExceptionClear(void *env) { (void)env; }
static void *jni_ExceptionOccurred(void *env) {
  (void)env;
  return 0;
}

/* Array */
static jint jni_GetArrayLength(void *env, void *array) {
  (void)env;
  if (array == (void *)&g_doframe_args_sentinel) return 1;   /* doFrame: Object[1] */
  if (array == (void *)&g_handlemsg_args_sentinel) return 1; /* handleMessage: Object[1] */
  struct barr *b = barr_find(array);
  return b ? b->len : 0;
}
/* GetObjectArrayElement: args do doFrame -> o Long boxed (slot 0) */
static void *jni_GetObjectArrayElement(void *env, void *array, jint idx) {
  (void)env; (void)idx;
  if (array == (void *)&g_doframe_args_sentinel) return &g_long_box_sentinel;
  if (array == (void *)&g_handlemsg_args_sentinel) return &g_message_sentinel;
  return NULL;
}
/* int[] accessors (InputDevice IDs etc.) */
static void *jni_GetIntArrayElements(void *env, void *arr, void *isCopy) {
  (void)env; if (isCopy) *(unsigned char *)isCopy = 0;
  struct barr *b = barr_find(arr); return b ? b->buf : NULL;
}
static void jni_ReleaseIntArrayElements(void *env, void *arr, void *el, int m) {
  (void)env; (void)arr; (void)el; (void)m;
}
static void jni_GetIntArrayRegion(void *env, void *arr, int start, int len, void *buf) {
  (void)env; struct barr *b = barr_find(arr);
  if (b && buf && start >= 0 && (start + len) * 4 <= (b->len * 4 > 0 ? b->len * 4 : 0) + 4)
    memcpy(buf, b->buf + start * 4, len * 4);
}
static void *jni_NewIntArray(void *env, int len) { (void)env; return iarr_new(NULL, len); }

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
  /* GetEnv e' chamado milhares de vezes (cada thread/icall) -> silenciado */
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

/* ---- recon: RegisterNatives com log + STORAGE dos ponteiros ---- */
struct native_method { const char *name; const char *sig; void *fn; };
static struct native_method g_natives[512];
static int g_natives_count = 0;

void *jni_find_native(const char *name) {
  for (int i = 0; i < g_natives_count; i++)
    if (strcmp(g_natives[i].name, name) == 0) return g_natives[i].fn;
  return 0;
}

static int jni_RegisterNatives(void *env, void *clazz, const void *methods, int n) {
  (void)env; (void)clazz;
  debugPrintf("jni_shim: >> RegisterNatives(%d metodos)\n", n);
  const uintptr_t *m = (const uintptr_t *)methods;  /* {name, sig, fnPtr} x n */
  for (int i = 0; i < n && i < 128; i++) {
    const char *nm = (const char *)m[i * 3];
    const char *sg = (const char *)m[i * 3 + 1];
    void *fn = (void *)m[i * 3 + 2];
    debugPrintf("     [%d] %s %s  -> %p\n", i, nm ? nm : "?", sg ? sg : "?", fn);
    if (nm && g_natives_count < 512) {
      g_natives[g_natives_count].name = strdup(nm);  /* COPIA: nomes do jnibridge
        (invoke/delete) são transientes → ponteiro dangle → jni_find_native falhava */
      g_natives[g_natives_count].sig = sg;
      g_natives[g_natives_count].fn = fn;
      g_natives_count++;
    }
  }
  return 0;
}

/* ---- jnibridge proxy: EXECUTA Runnables postados (runOnUiThread/post) ----
 * O Cuphead (Unity Analytics/init) cria um Runnable via newInterfaceProxy(handle,...) e
 * faz runOnUiThread(runnable), depois faz poll esperando ele rodar. Sem looper o shim
 * era no-op → o Runnable nunca rodava → boot travava. Aqui guardamos proxy→handle e, no
 * runOnUiThread, chamamos o native "invoke" do jnibridge SÍNCRONO (roda o delegate C#). */
static void proxy_register(void *obj, long h) {
  if (g_proxy_n < 512) { g_proxies[g_proxy_n].obj = obj; g_proxies[g_proxy_n].handle = h; g_proxy_n++; }
}
static long proxy_handle(void *obj) {
  for (int i = g_proxy_n - 1; i >= 0; i--) if (g_proxies[i].obj == obj) return g_proxies[i].handle;
  return 0;
}
static _Thread_local int g_in_run;
int jni_is_run_method(void *o) { return o == (void *)&g_run_method_sentinel; }
int jni_is_empty_args(void *o) { return o == (void *)&g_empty_args_sentinel; }
static void run_runnable(void *env, void *runnable) {
  if (!runnable) return;
  if (g_in_run >= 6) { debugPrintf("jni_shim: runOnUiThread anti-recursao\n"); return; }
  long h = proxy_handle(runnable);
  void *invoke = jni_find_native("invoke");
  if (!h || !invoke) { debugPrintf("jni_shim: runOnUiThread sem handle/invoke (r=%p h=%ld invoke=%p natives=%d)\n", runnable, h, invoke, g_natives_count); return; }
  g_in_run++;
  debugPrintf("jni_shim: >> RODANDO Runnable (handle=%ld) ...\n", h);
  ((void *(*)(void *, void *, long, void *, void *, void *))invoke)(
      env, &g_runnable_class_sentinel, h, &g_runnable_class_sentinel,
      &g_run_method_sentinel, &g_empty_args_sentinel);
  debugPrintf("jni_shim: << Runnable terminou (handle=%ld)\n", h);
  g_in_run--;
}

/* ---- Choreographer: dispara FrameCallback.doFrame(frameTimeNanos) ----
 * Mesmo caminho do run_runnable, mas com o Method "doFrame" + args = Object[1]{Long}.
 * Chamado pela driver-thread (main.c) ~60Hz. Retorna 1 se disparou, 0 se ainda não há
 * FrameCallback capturado. g_choreo_log liga log detalhado das queries (1ª vez). */
int g_choreo_log = 0;
int jni_choreo_doframe(void *env, long nanos) {
  void *proxy = g_framecb_proxy;
  if (!proxy) return 0;
  long h = proxy_handle(proxy);
  void *invoke = jni_find_native("invoke");
  if (!h || !invoke) return 0;
  g_doframe_nanos = nanos;
  static int once = 0;
  if (g_choreo_log && !once) { once = 1; debugPrintf("jni_shim: [CHOREO] 1º doFrame(handle=%ld nanos=%ld)\n", h, nanos); }
  ((void *(*)(void *, void *, long, void *, void *, void *))invoke)(
      env, &g_runnable_class_sentinel, h, &g_runnable_class_sentinel,
      &g_doframe_method_sentinel, &g_doframe_args_sentinel);
  return 1;
}
void *jni_shim_env(void) { return jni_env_ptr; }
int jni_choreo_captured(void) { return g_framecb_proxy != NULL; }

/* ---- Handler$Callback.handleMessage(Message): invoca o delegate C# do proxy ----
 * Mesmo caminho do doFrame, com Method "handleMessage" + args Object[1]{Message}. O proxy
 * (g_framecb_proxy) implementa Handler$Callback E Choreographer$FrameCallback (mesmo handle). */
void jni_handlemessage(void *env) {
  void *proxy = g_framecb_proxy;
  if (!proxy) { debugPrintf("jni_shim: handleMessage sem proxy capturado\n"); return; }
  long h = proxy_handle(proxy);
  void *invoke = jni_find_native("invoke");
  if (!h || !invoke) { debugPrintf("jni_shim: handleMessage sem handle/invoke (h=%ld invoke=%p)\n", h, invoke); return; }
  debugPrintf("jni_shim: >> handleMessage(what=%d handle=%ld) ...\n", g_message_what, h);
  ((void *(*)(void *, void *, long, void *, void *, void *))invoke)(
      env, &g_runnable_class_sentinel, h, &g_runnable_class_sentinel,
      &g_handlemsg_method_sentinel, &g_handlemsg_args_sentinel);
  debugPrintf("jni_shim: << handleMessage terminou\n");
}

/* ---- DirectByteBuffer p/ a thread de áudio do FMOD (AudioTrack Java) ----
   fmodProcess(env, thiz, ByteBuffer) faz GetDirectBufferAddress/Capacity no buffer
   p/ saber onde escrever o PCM. Damos um buffer real nosso. */
static unsigned char g_fmod_pcm[32768];
static int g_fmod_bb_sentinel;
/* 🔑 CAPACIDADE do DirectByteBuffer = nº de bytes que fmodProcess PREENCHE por chamada
   (FMOD enche o buffer inteiro e AVANÇA o clock do mixer nesse tanto de frames). ISSO PRECISA
   CASAR com os bytes que o pump (fmod_audio_thread) enfileira no SDL. Antes reportávamos 32768
   (8192 frames) mas o pump só enfileirava 4096 (1024 frames) -> o FMOD avançava 8x mais rápido
   que o playback -> ÁUDIO ACELERADO. 4096 = bloco DSP padrão do FMOD mobile (1024 frames
   stereo s16). Tunável por TER_AUDIO_BUF (bytes). O backing g_fmod_pcm[32768] é só folga. */
int g_fmod_cap = 4096;
void *jni_fmod_bytebuffer(void) { return &g_fmod_bb_sentinel; }
void *jni_fmod_pcm(void) { return g_fmod_pcm; }
int jni_fmod_pcm_size(void) { return g_fmod_cap; }
static void *jni_GetDirectBufferAddress(void *env, void *buf) {
  (void)env; if (buf == &g_fmod_bb_sentinel) return g_fmod_pcm; return NULL;
}
static long jni_GetDirectBufferCapacity(void *env, void *buf) {
  (void)env; if (buf == &g_fmod_bb_sentinel) return (long)g_fmod_cap; return -1;
}
/* org.fmod.FMODAudioDevice — FMOD faz NewObject(FMODAudioDevice) e chama start() p/ subir o
   AudioTrack. Sem isso (NewObject→NULL via jni_stub) o System::init do FMOD dá erro 60. Damos
   um device fake não-nulo + métodos (start/etc.) OK; a thread C (fmod_audio_thread) bombeia
   fmodProcess no lugar da thread Java. */
void *jni_fmod_device(void) { return &g_fmod_device_obj; }
static void *jni_NewObject(void *env, void *clazz, void *mid, ...) {
  (void)env; (void)mid;
  if (clazz == class_for("org/fmod/FMODAudioDevice")) {
    debugPrintf("jni_shim: NewObject(FMODAudioDevice) -> device fake\n"); return &g_fmod_device_obj;
  }
  return NULL;   /* comportamento atual (jni_stub=0) p/ as demais classes — sem regressão */
}
static void *jni_NewObjectV(void *env, void *clazz, void *mid, va_list ap){ (void)ap; return jni_NewObject(env,clazz,mid); }
static void *jni_NewObjectA(void *env, void *clazz, void *mid, void *args){ (void)args; return jni_NewObject(env,clazz,mid); }

/* GetJavaVM (index 219) — initJni chama isso */
static jint jni_GetJavaVM(void *env, void **vm) {
  (void)env;
  debugPrintf("jni_shim: GetJavaVM -> nossa VM\n");
  *vm = &java_vm_ptr;   /* mesma JavaVM passada no out_vm */
  return 0;
}

/* ---- Init ---- */

void jni_install_indexed(uintptr_t *vt, int n);

void jni_shim_init(void **out_vm, void **out_env) {
  if (getenv("TER_AUDIO_BUF")) { int v = atoi(getenv("TER_AUDIO_BUF"));
    if (v >= 512 && v <= 32768) g_fmod_cap = v & ~3; }   /* casa capacidade do BB com o que o pump enfileira */
  for (int i = 0; i < JNI_VTABLE_SIZE; i++) {
    jni_env_vtable[i] = (uintptr_t)jni_stub;
    java_vm_vtable[i] = (uintptr_t)jni_stub;
  }
  jni_install_indexed(jni_env_vtable, JNI_VTABLE_SIZE);

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
  jni_env_vtable[215] = (uintptr_t)jni_RegisterNatives;  /* recon: Unity */
  jni_env_vtable[219] = (uintptr_t)jni_GetJavaVM;        /* recon: Unity initJni */
  /* AssetManager bridge: byte-array functions */
  jni_env_vtable[171] = (uintptr_t)jni_GetArrayLength_real;
  jni_env_vtable[176] = (uintptr_t)jni_NewByteArray;
  jni_env_vtable[184] = (uintptr_t)jni_GetByteArrayElements;
  jni_env_vtable[192] = (uintptr_t)jni_ReleaseByteArrayElements;
  jni_env_vtable[200] = (uintptr_t)jni_GetByteArrayRegion;
  jni_env_vtable[208] = (uintptr_t)jni_SetByteArrayRegion;
  jni_env_vtable[15] = (uintptr_t)jni_ExceptionOccurred;
  jni_env_vtable[17] = (uintptr_t)jni_ExceptionClear;
  jni_env_vtable[21] = (uintptr_t)jni_NewGlobalRef;
  jni_env_vtable[22] = (uintptr_t)jni_DeleteGlobalRef;
  jni_env_vtable[23] = (uintptr_t)jni_DeleteLocalRef;
  jni_env_vtable[25] = (uintptr_t)jni_NewLocalRef;
  jni_env_vtable[24] = (uintptr_t)jni_IsSameObject;
  jni_env_vtable[28] = (uintptr_t)jni_NewObject;    /* NewObject (varargs) — FMODAudioDevice */
  jni_env_vtable[29] = (uintptr_t)jni_NewObjectV;   /* NewObjectV (va_list) */
  jni_env_vtable[30] = (uintptr_t)jni_NewObjectA;   /* NewObjectA (jvalue*) */
  jni_env_vtable[31] = (uintptr_t)jni_GetObjectClass;
  jni_env_vtable[32] = (uintptr_t)jni_IsInstanceOf;
  jni_env_vtable[33] = (uintptr_t)jni_GetMethodID;
  jni_env_vtable[52] = (uintptr_t)jni_CallLongMethod;
  jni_env_vtable[53] = (uintptr_t)jni_CallLongMethodV;
  jni_env_vtable[34] = (uintptr_t)jni_CallObjectMethod;
  jni_env_vtable[35] = (uintptr_t)jni_CallObjectMethodV;   /* V variant (va_list) */
  jni_env_vtable[37] = (uintptr_t)jni_CallBooleanMethod;
  jni_env_vtable[38] = (uintptr_t)jni_CallBooleanMethodV;  /* V (va_list) */
  jni_env_vtable[49] = (uintptr_t)jni_CallIntMethod;
  jni_env_vtable[50] = (uintptr_t)jni_CallIntMethodV;      /* V (va_list) */
  jni_env_vtable[55] = (uintptr_t)jni_CallFloatMethod;     /* getRefreshRate */
  jni_env_vtable[56] = (uintptr_t)jni_CallFloatMethodV;    /* V */
  jni_env_vtable[100] = (uintptr_t)jni_GetIntField;        /* DisplayMetrics int fields */
  jni_env_vtable[102] = (uintptr_t)jni_GetFloatField;      /* DisplayMetrics float fields */
  jni_env_vtable[61] = (uintptr_t)jni_CallVoidMethod;
  jni_env_vtable[62] = (uintptr_t)jni_CallVoidMethodV;     /* V (va_list) */
  jni_env_vtable[94] = (uintptr_t)jni_GetFieldID;
  jni_env_vtable[113] = (uintptr_t)jni_GetStaticMethodID;
  jni_env_vtable[114] = (uintptr_t)jni_CallStaticObjectMethod;
  jni_env_vtable[115] = (uintptr_t)jni_CallStaticObjectMethodV; /* V (va_list) */
  jni_env_vtable[117] = (uintptr_t)jni_CallStaticBooleanMethod;
  jni_env_vtable[118] = (uintptr_t)jni_CallStaticBooleanMethod; /* V */
  jni_env_vtable[129] = (uintptr_t)jni_CallStaticIntMethod;
  jni_env_vtable[130] = (uintptr_t)jni_CallStaticIntMethodV; /* V (va_list) */
  jni_env_vtable[141] = (uintptr_t)jni_CallStaticVoidMethod;
  jni_env_vtable[142] = (uintptr_t)jni_CallStaticVoidMethod; /* V */
  jni_env_vtable[144] = (uintptr_t)jni_GetStaticFieldID;
  jni_env_vtable[145] = (uintptr_t)jni_GetStaticObjectField;
  jni_env_vtable[150] = (uintptr_t)jni_GetStaticIntField;
  jni_env_vtable[167] = (uintptr_t)jni_NewStringUTF;
  jni_env_vtable[168] = (uintptr_t)jni_GetStringUTFLength;
  jni_env_vtable[169] = (uintptr_t)jni_GetStringUTFChars;
  jni_env_vtable[170] = (uintptr_t)jni_ReleaseStringUTFChars;
  jni_env_vtable[171] = (uintptr_t)jni_GetArrayLength;
  jni_env_vtable[173] = (uintptr_t)jni_GetObjectArrayElement; /* doFrame args[0]=Long */
  jni_env_vtable[179] = (uintptr_t)jni_NewIntArray;          /* NewIntArray */
  jni_env_vtable[187] = (uintptr_t)jni_GetIntArrayElements;  /* int[] elements */
  jni_env_vtable[195] = (uintptr_t)jni_ReleaseIntArrayElements;
  jni_env_vtable[203] = (uintptr_t)jni_GetIntArrayRegion;
  jni_env_vtable[205] = (uintptr_t)jni_ExceptionCheck;
  jni_env_vtable[230] = (uintptr_t)jni_GetDirectBufferAddress;
  jni_env_vtable[231] = (uintptr_t)jni_GetDirectBufferCapacity;

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

/* lista todos os métodos nativos registrados via RegisterNatives (debug F1) */
extern void jni_dump_natives(void);
void jni_dump_natives(void) {
  fprintf(stderr, "[NATIVES] %d métodos registrados:\n", g_natives_count);
  for (int i = 0; i < g_natives_count; i++)
    fprintf(stderr, "  %s = %p\n", g_natives[i].name, g_natives[i].fn);
}
