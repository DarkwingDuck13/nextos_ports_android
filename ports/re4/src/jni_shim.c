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
static const char *g_package_name = "com.WS.RE4";
static int g_obb_version = 1;

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

/* ---- SharedPreferences store REAL (key->value) ----
   RE4 mobile guarda "sceneToLoad" (e outros) via SharedPreferences.Editor.putString e LE de volta
   com getString. Sem armazenar, getString devolve o default -> a cena a carregar fica vazia -> o
   loader volta pro menu (gameplay nunca inicia). Aqui mantemos um mapa em memoria (sessao). */
#define PREFS_MAX 256
static struct { char *key; char *val; int isint; long ival; } g_prefs[PREFS_MAX];
static int g_prefs_n = 0;
static int g_prefs_editor;   /* sentinela do SharedPreferences.Editor (putString retorna isto) */
static int prefs_find(const char *k){ if(!k)return -1; for(int i=0;i<g_prefs_n;i++) if(g_prefs[i].key&&strcmp(g_prefs[i].key,k)==0) return i; return -1; }
extern void re4_signal_gameplay(int on);  /* main_re4: liga gameplay (anti-freeze + touch-move) */
static void prefs_set_str(const char *k, const char *v){ if(!k)return; int i=prefs_find(k);
  if(i<0){ if(g_prefs_n>=PREFS_MAX)return; i=g_prefs_n++; g_prefs[i].key=strdup(k); g_prefs[i].val=NULL; }
  if(g_prefs[i].val) free(g_prefs[i].val); g_prefs[i].val=strdup(v?v:""); g_prefs[i].isint=0;
  debugPrintf("[PREFS] set str '%s'='%s'\n", k, v?v:"");
  /* sceneToLoad com valor de nivel = entrou no gameplay; vazio/0 = voltou ao menu */
  if(strcmp(k,"sceneToLoad")==0) re4_signal_gameplay(v && v[0] && strcmp(v,"0")!=0); }
static void prefs_set_int(const char *k, long v){ if(!k)return; int i=prefs_find(k);
  if(i<0){ if(g_prefs_n>=PREFS_MAX)return; i=g_prefs_n++; g_prefs[i].key=strdup(k); g_prefs[i].val=NULL; }
  g_prefs[i].ival=v; g_prefs[i].isint=1;
  debugPrintf("[PREFS] set int '%s'=%ld\n", k, v); }
static const char *prefs_get_str(const char *k){ int i=prefs_find(k); return (i>=0&&!g_prefs[i].isint&&g_prefs[i].val)?g_prefs[i].val:NULL; }
static int prefs_has_int(const char *k, long *out){ int i=prefs_find(k); if(i>=0&&g_prefs[i].isint){ if(out)*out=g_prefs[i].ival; return 1; } return 0; }
static void prefs_remove(const char *k){ int i=prefs_find(k); if(i<0)return; free(g_prefs[i].key); if(g_prefs[i].val)free(g_prefs[i].val);
  for(int j=i;j<g_prefs_n-1;j++) g_prefs[j]=g_prefs[j+1]; g_prefs_n--; }
void *jni_prefs_editor(void){ return &g_prefs_editor; }

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

static const char *re4_gamedir(void) {
  const char *dir = getenv("RE4_GAMEDIR");
  return (dir && dir[0]) ? dir : "/storage/roms/ports/re4";
}

static const char *re4_userdata_dir(void) {
  const char *dir = getenv("RE4_USERDATA");
  static char path[1024];
  if (dir && dir[0]) return dir;
  snprintf(path, sizeof(path), "%s/userdata", re4_gamedir());
  return path;
}

static const char *re4_assets_dir(void) {
  const char *dir = getenv("RE4_ASSETDIR");
  static char path[1024];
  if (dir && dir[0]) return dir;
  snprintf(path, sizeof(path), "%s/assets", re4_gamedir());
  return path;
}

/* ===================================================================
 * AssetManager bridge — le de <RE4_GAMEDIR>/assets/<path>
 * (Unity: getAssets() + AssetManager.open(path) + InputStream.read/close)
 * =================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

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

/* --- InputStream (FILE*) tracking --- */
#define MAX_ASTREAMS 32
struct astream { FILE *fp; long size; };
static struct astream g_astreams[MAX_ASTREAMS];
static int g_astream_n = 0;
static void *asset_open(const char *path) {
  char full[1200];
  snprintf(full, sizeof(full), "%s/%s", re4_assets_dir(), path ? path : "");
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

/* --- int-array (InputManager.getDeviceIds() -> int[]) --- */
#define MAX_IARR 8
struct iarr { int *buf; int len; };
static struct iarr g_iarr[MAX_IARR];
static int g_iarr_n = 0;
static void *iarr_new(const int *data, int len) {
  int i = g_iarr_n++ % MAX_IARR;
  if (g_iarr[i].buf) free(g_iarr[i].buf);
  g_iarr[i].buf = (int *)malloc((len > 0 ? len : 1) * sizeof(int));
  g_iarr[i].len = len;
  if (data && len > 0 && g_iarr[i].buf) memcpy(g_iarr[i].buf, data, (size_t)len * sizeof(int));
  return &g_iarr[i];
}
static struct iarr *iarr_find(void *h) {
  if ((char *)h >= (char *)g_iarr && (char *)h < (char *)(g_iarr + MAX_IARR))
    return (struct iarr *)h;
  return NULL;
}
static void *jni_GetIntArrayElements(void *env, void *arr, void *isCopy) {
  (void)env; if (isCopy) *(unsigned char *)isCopy = 0;
  struct iarr *ia = iarr_find(arr); return ia ? ia->buf : NULL;
}
static void jni_ReleaseIntArrayElements(void *env, void *arr, void *elems, int mode) {
  (void)env; (void)arr; (void)elems; (void)mode;
}
static void jni_GetIntArrayRegion(void *env, void *arr, int start, int len, void *buf) {
  (void)env; struct iarr *ia = iarr_find(arr);
  if (ia && start >= 0 && len >= 0 && start + len <= ia->len)
    memcpy(buf, ia->buf + start, (size_t)len * sizeof(int));
}

/* sentinelas do gamepad (InputDevice / MotionRange) p/ o jogo registrar o controle */
static int g_obj_inputdevice;
static int g_obj_motionrange;
static int g_motionrange_axis = 0;  /* eixo do ultimo getMotionRange(axis) */
#define RE4_PAD_DEVICE_ID 1   /* id do gamepad virtual; bate com o deviceId do MotionEvent injetado */

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
struct hk_inject_s g_hk_inject;       /* exportado p/ main_recon */
static int g_obj_keyevent;            /* sentinela do objeto KeyEvent */
void *hk_keyevent_object(void) { return &g_obj_keyevent; }
static int g_obj_motionevent;         /* sentinela do objeto MotionEvent (stick analogico) */
void *hk_motionevent_object(void) { return &g_obj_motionevent; }

/* sentinela do org.fmod.FMODAudioDevice (NewObject + métodos init/start do FMOD).
   Sem isto NewObject->NULL -> System::init do FMOD da "Error initializing output device (60)". */
int g_fmod_device_obj;
void *jni_fmod_device(void) { return &g_fmod_device_obj; }

/* classes distintas por nome (Unity compara KeyEvent.class vs MotionEvent.class) */
static struct { const char *name; int tag; } g_classreg[128];
static int g_classreg_n = 0;
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
  return &g_method_tags[FID_GENERIC];
}

static void *jni_GetStaticFieldID(void *env, void *clazz, const char *name,
                                  const char *sig) {
  (void)env;
  (void)clazz;
  debugPrintf("jni_shim: GetStaticFieldID(%s, %s)\n", name, sig);
  if (strcmp(name, "OBB_VERSIONCODE") == 0)
    return &g_method_tags[FID_OBB_VERSIONCODE];
  return &g_method_tags[FID_GENERIC];
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
    /* diretorios de dados -> path REAL gravavel (persistentDataPath do Unity).
       Sem isso (=""), PlayerPrefs/save quebram -> jogo trava em "first run". */
    if (strcmp(nm, "getFilesDir") == 0 || strcmp(nm, "getExternalFilesDir") == 0 ||
        strcmp(nm, "getCacheDir") == 0 || strcmp(nm, "getExternalCacheDir") == 0 ||
        strcmp(nm, "getDataDir") == 0 || strcmp(nm, "getExternalStorageDirectory") == 0 ||
        strcmp(nm, "getPath") == 0 || strcmp(nm, "getAbsolutePath") == 0 ||
        strcmp(nm, "getCanonicalPath") == 0)
      return make_jstring(re4_userdata_dir());
    /* SharedPreferences.edit() -> Editor (mesmo obj p/ encadear puts e commit) */
    if (strcmp(nm, "edit") == 0) return &g_prefs_editor;
    /* SharedPreferences.getString(key, default) -> valor armazenado ou default */
    if (strcmp(nm, "getString") == 0) {
      void *keystr = va_arg(ap, void *);
      void *defstr = va_arg(ap, void *);
      const char *k = resolve_jstring(keystr);
      const char *v = prefs_get_str(k);
      debugPrintf("[PREFS] getString key='%s' -> '%s'\n", k, v ? v : (defstr?resolve_jstring(defstr):"(def null)"));
      if (v) return make_jstring(v);
      return defstr ? defstr : make_jstring("");
    }
    /* SharedPreferences.Editor.putString(key, val) -> armazena, retorna o Editor (encadeavel) */
    if (strcmp(nm, "putString") == 0) {
      void *keystr = va_arg(ap, void *);
      void *valstr = va_arg(ap, void *);
      prefs_set_str(resolve_jstring(keystr), resolve_jstring(valstr));
      return &g_prefs_editor;
    }
    /* Editor.putInt/putFloat/putBoolean/putLong tambem retornam o Editor */
    if (strcmp(nm, "putInt") == 0 || strcmp(nm, "putLong") == 0) {
      void *keystr = va_arg(ap, void *);
      long v = va_arg(ap, long);
      prefs_set_int(resolve_jstring(keystr), v);
      return &g_prefs_editor;
    }
    if (strcmp(nm, "putFloat") == 0 || strcmp(nm, "putBoolean") == 0) {
      (void)va_arg(ap, void *);  /* key */
      return &g_prefs_editor;    /* armazenamento simples: ignora valor, mantem encadeamento */
    }
    /* Editor.remove(key) / clear() -> retorna o Editor */
    if (strcmp(nm, "remove") == 0) { void *keystr = va_arg(ap, void *); prefs_remove(resolve_jstring(keystr)); return &g_prefs_editor; }
    if (strcmp(nm, "clear") == 0) { g_prefs_n = 0; return &g_prefs_editor; }
    if (strcmp(nm, "toString") == 0)
      return make_jstring("");
    /* GAMEPAD: InputDevice.* — caracteriza o controle p/ o jogo habilitar os eixos */
    if (obj == &g_obj_inputdevice) {
      if (strcmp(nm, "getName") == 0) return make_jstring("Xbox Controller");
      if (strcmp(nm, "getDescriptor") == 0) return make_jstring("re4pad0");
      if (strcmp(nm, "getMotionRange") == 0) { g_motionrange_axis = va_arg(ap, int); return &g_obj_motionrange; }
      if (strcmp(nm, "getMotionRanges") == 0) return &g_empty_list; /* lista (size=0); eixos vem por getAxisValue */
      if (strcmp(nm, "getVibrator") == 0) return &fake_obj;
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

/* CallBooleanMethod (index 49) */
static unsigned char jni_CallBooleanMethod(void *env, void *obj,
                                           void *methodID, ...) {
  (void)env;
  (void)obj;
  const char *nm = mid_name(methodID);
  /* FMODAudioDevice.init(...)/start()/etc. = sucesso (true) -> output do FMOD inicializa */
  if (obj == &g_fmod_device_obj) return 1;
  if (nm) {
    if (strcmp(nm, "isEmpty") == 0) return 1;  /* lista vazia */
    if (strcmp(nm, "hasNext") == 0) return 0;  /* iterator vazio */
    if (strcmp(nm, "commit") == 0) return 1;   /* SharedPreferences.Editor.commit() -> true */
    if (strcmp(nm, "contains") == 0) return 0; /* contains(key): default ausente */
    if (strcmp(nm, "getBoolean") == 0) return 0;
  }
  return 0;
}

/* CallIntMethod — variante V */
static jint jni_CallIntMethodV(void *env, void *obj, void *methodID,
                               va_list ap) {
  (void)env;
  const char *nm = mid_name(methodID);
  /* FMODAudioDevice.* (init/getInfo/...) chamado pelo C do FMOD = sucesso (1) */
  if (obj == &g_fmod_device_obj) { debugPrintf("jni_shim: FMODAudioDevice.%s -> 1\n", nm?nm:"?"); return 1; }
  if (nm) {
    /* ---- KeyEvent (nativeInjectEvent) ---- */
    if (strcmp(nm, "getAction") == 0) { debugPrintf("[KEYEV] getAction->%d\n", g_hk_inject.action); return g_hk_inject.action; }
    if (strcmp(nm, "getKeyCode") == 0) { debugPrintf("[KEYEV] getKeyCode->%d\n", g_hk_inject.keycode); return g_hk_inject.keycode; }
    if (strcmp(nm, "getSource") == 0) return g_hk_inject.source;
    if (strcmp(nm, "getDeviceId") == 0) return g_hk_inject.deviceId;
    if (strcmp(nm, "getMetaState") == 0) return g_hk_inject.metaState;
    if (strcmp(nm, "getRepeatCount") == 0) return g_hk_inject.repeat;
    if (strcmp(nm, "getScanCode") == 0) return g_hk_inject.scancode;
    if (strcmp(nm, "getInt") == 0) { void *k = va_arg(ap, void *); int d = va_arg(ap, int);
      long sv; if (prefs_has_int(resolve_jstring(k), &sv)) { debugPrintf("[PREFS] getInt key='%s' -> %ld\n", resolve_jstring(k), sv); return (jint)sv; }
      debugPrintf("[PREFS] getInt key='%s' def=%d\n", resolve_jstring(k), d); return d; }
    if (strcmp(nm, "getFlags") == 0) return g_hk_inject.flags;
    if (strcmp(nm, "getUnicodeChar") == 0) return g_hk_inject.unicode;
    /* GAMEPAD: InputDevice.* int methods — fonte JOYSTICK|GAMEPAD|DPAD faz o jogo tratar
       como controle e ler os eixos. getId casa com o deviceId do MotionEvent injetado. */
    if (obj == &g_obj_inputdevice) {
      if (strcmp(nm, "getSources") == 0) return 0x01000010 | 0x00000401 | 0x00000201; /* JOYSTICK|GAMEPAD|DPAD */
      if (strcmp(nm, "getId") == 0) return RE4_PAD_DEVICE_ID;
      if (strcmp(nm, "getControllerNumber") == 0) return 1;
      if (strcmp(nm, "getVendorId") == 0) return 0x045e;  /* Microsoft */
      if (strcmp(nm, "getProductId") == 0) return 0x028e; /* Xbox 360 pad */
      if (strcmp(nm, "getKeyboardType") == 0) return 0;
      if (strcmp(nm, "supportsSource") == 0) return 1;
    }
    if (obj == &g_obj_motionrange) {
      if (strcmp(nm, "getAxis") == 0) return g_motionrange_axis;
      if (strcmp(nm, "getSource") == 0) return 0x01000010; /* JOYSTICK */
    }
    /* MotionEvent injetado (stick): o jogo pergunta getPointerCount ANTES de ler eixos;
       0 -> ignora os eixos -> sem movimento. Retornamos 1 + metadados coerentes. LOG p/ ver
       exatamente quais metodos o jogo chama no evento (descobrir o caminho real do analogico). */
    if (obj == &g_obj_motionevent) {
      static int ml=0; if(ml++<80) debugPrintf("[MOTIONM-int] %s\n", nm?nm:"?");
      if (strcmp(nm, "getPointerCount") == 0) return 1;
      if (strcmp(nm, "getPointerId") == 0) return 0;
      if (strcmp(nm, "getHistorySize") == 0) return 0;
      if (strcmp(nm, "getActionMasked") == 0) return g_hk_inject.action;
      if (strcmp(nm, "getActionIndex") == 0) return 0;
      if (strcmp(nm, "getButtonState") == 0) return 0;
      if (strcmp(nm, "getEdgeFlags") == 0) return 0;
      if (strcmp(nm, "findPointerIndex") == 0) return 0;
      if (strcmp(nm, "getToolType") == 0) return 0;
    }
    if (strcmp(nm, "size") == 0) return 0; /* List/Collection vazia */
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
static void jni_CallVoidMethod(void *env, void *obj, void *methodID, ...) {
  (void)env;
  const char *nm = mid_name(methodID);
  struct astream *s = astream_find(obj);
  if (s && nm && strcmp(nm, "close") == 0) {
    if (s->fp) { fclose(s->fp); s->fp = NULL; }
  }
}

/* CallStaticObjectMethod — variante V (va_list, usada pelo il2cpp/Unity) */
static void *jni_CallStaticObjectMethodV(void *env, void *clazz,
                                         void *methodID, va_list ap) {
  (void)env;
  (void)clazz;
  const char *nm = mid_name(methodID);
  debugPrintf("jni_shim: CallStaticObjectMethod(%s)\n", nm ? nm : "?");
  /* encode/decode (String)->String: o RE4 OFUSCA chave+valor do SharedPreferences (sceneToLoad
     etc.) com encode() antes de gravar e decode() ao ler. IDENTIDADE (devolve o argumento)
     mantem o round-trip exato (encode e decode sao inversos) -> a cena a carregar nao se perde. */
  if (nm && (strcmp(nm, "encode") == 0 || strcmp(nm, "decode") == 0)) {
    void *arg = va_arg(ap, void *);
    return arg ? arg : make_jstring("");
  }
  /* GAMEPAD: InputDevice.getDeviceIds() -> int[]{id} (o jogo faz polling disto p/ achar
     controles; sem um int[] valido ele conclui "sem gamepad" e nunca le getAxisValue). */
  if (nm && strcmp(nm, "getDeviceIds") == 0) {
    static const int ids[1] = { RE4_PAD_DEVICE_ID };
    return iarr_new(ids, 1);
  }
  if (nm && strcmp(nm, "getDevice") == 0) {
    return &g_obj_inputdevice;  /* InputDevice nao-nulo */
  }
  static int fake_result;
  return &fake_result;  /* fake Class/objeto nao-nulo (forName etc.) */
}
static void *jni_CallStaticObjectMethod(void *env, void *clazz,
                                        void *methodID, ...) {
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
static jint jni_CallStaticIntMethod(void *env, void *clazz, void *methodID,
                                    ...) {
  (void)env;
  (void)clazz;
  (void)methodID;
  return 0;
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
  debugPrintf("jni_shim: GetStaticIntField(fid=%p) -> 0\n", fieldID);
  return 0;
}

/* GetStaticObjectField (index 156) */
static void *jni_GetStaticObjectField(void *env, void *clazz, void *fieldID) {
  (void)env;
  (void)clazz;
  (void)fieldID;
  debugPrintf("jni_shim: GetStaticObjectField -> NULL\n");
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
  if (obj == &g_obj_keyevent) return class_for("android/view/KeyEvent");
  if (obj == &g_obj_motionevent) return class_for("android/view/MotionEvent");
  static int fake_obj_class;
  return &fake_obj_class;
}
static unsigned char jni_IsInstanceOf(void *env, void *obj, void *clazz) {
  (void)env;
  if (obj == &g_obj_keyevent) {
    return clazz == class_for("android/view/KeyEvent") ||
           clazz == class_for("android/view/InputEvent") ||
           clazz == class_for("java/lang/Object");
  }
  if (obj == &g_obj_motionevent) {
    /* MotionEvent NAO e KeyEvent: o nativeInjectEvent ramifica por instanceof -> precisa
       distinguir, senao o motion seria lido como tecla (keycode lixo). */
    return clazz == class_for("android/view/MotionEvent") ||
           clazz == class_for("android/view/InputEvent") ||
           clazz == class_for("java/lang/Object");
  }
  return 1; /* permissivo p/ outros casts */
}
static unsigned char jni_IsSameObject(void *env, void *a, void *b) {
  (void)env; return a == b;
}
/* CallLongMethod V — getEventTime/getDownTime do KeyEvent (retornam long) */
static long jni_CallLongMethodV(void *env, void *obj, void *methodID, va_list ap) {
  (void)env; (void)obj; (void)ap;
  const char *nm = mid_name(methodID);
  if (nm) {
    if (strcmp(nm, "getEventTime") == 0) return g_hk_inject.eventTime;
    if (strcmp(nm, "getDownTime") == 0) return g_hk_inject.downTime;
  }
  return 0;
}
static long jni_CallLongMethod(void *env, void *obj, void *methodID, ...) {
  va_list ap; va_start(ap, methodID);
  long r = jni_CallLongMethodV(env, obj, methodID, ap);
  va_end(ap);
  return r;
}

/* CallFloatMethod V — MotionEvent.getAxisValue(axis) do gamepad analogico.
   nativeInjectEvent recebe nosso MotionEvent e le os eixos por getAxisValue -> Leon anda
   (stick esq) + camera/mira (stick dir). Sem isto o slot caia no jni_stub (lixo no s0). */
static float jni_CallFloatMethodV(void *env, void *obj, void *methodID, va_list ap) {
  (void)env; (void)obj;
  const char *nm = mid_name(methodID);
  if (obj == &g_obj_motionevent) { static int fl=0; if(fl++<80) debugPrintf("[MOTIONM-flt] %s\n", nm?nm:"?"); }
  if (nm) {
    if (strcmp(nm, "getAxisValue") == 0) {
      int axis = va_arg(ap, int);              /* getAxisValue(int axis [, int pointer]) */
      float v = (axis >= 0 && axis < 64) ? g_hk_inject.axes[axis] : 0.0f;
      static int lg = 0; if (lg++ < 40) debugPrintf("[MOTION] getAxisValue(%d)->%.3f\n", axis, v);
      return v;
    }
    if (strcmp(nm, "getX") == 0) return g_hk_inject.axes[0];   /* AXIS_X */
    if (strcmp(nm, "getY") == 0) return g_hk_inject.axes[1];   /* AXIS_Y */
    /* MotionRange.getMin/getMax/getFlat/getFuzz: eixo normalizado [-1,1] */
    if (strcmp(nm, "getMin") == 0) return -1.0f;
    if (strcmp(nm, "getMax") == 0) return 1.0f;
    if (strcmp(nm, "getRange") == 0) return 2.0f;
    if (strcmp(nm, "getFlat") == 0 || strcmp(nm, "getFuzz") == 0) return 0.0f;
  }
  return 0.0f;
}
static float jni_CallFloatMethod(void *env, void *obj, void *methodID, ...) {
  va_list ap; va_start(ap, methodID);
  float r = jni_CallFloatMethodV(env, obj, methodID, ap);
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
  struct iarr *ia = iarr_find(array); if (ia) return ia->len;   /* getDeviceIds int[] */
  struct barr *b = barr_find(array);  if (b) return b->len;     /* byte[] (AssetManager) */
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
      g_natives[g_natives_count].name = nm;
      g_natives[g_natives_count].sig = sg;
      g_natives[g_natives_count].fn = fn;
      g_natives_count++;
    }
  }
  return 0;
}

/* ---- FMOD AudioTrack output: NewObject(FMODAudioDevice) + DirectByteBuffer ----
   O FMOD (dentro da libunity) usa o output AUDIOTRACK: FindClass(org/fmod/FMODAudioDevice),
   NewObject, e uma thread Java chamaria fmodProcess(ByteBuffer) p/ encher PCM. Aqui damos um
   device fake + um DirectByteBuffer real; a thread C (fmod_audio_thread em main_re4) bombeia
   fmodProcess no lugar da thread Java -> SOM + destrava o wait do menu (audio nunca sinalizava). */
static unsigned char g_fmod_pcm[32768];
static int g_fmod_bb_sentinel;
int g_fmod_cap = 4096;   /* bytes que fmodProcess preenche/que o pump enfileira (casar = ritmo certo) */
void *jni_fmod_bytebuffer(void) { return &g_fmod_bb_sentinel; }
void *jni_fmod_pcm(void) { return g_fmod_pcm; }
int jni_fmod_pcm_size(void) { return g_fmod_cap; }
static void *jni_GetDirectBufferAddress(void *env, void *buf) {
  (void)env; if (buf == &g_fmod_bb_sentinel) return g_fmod_pcm; return NULL;
}
static long jni_GetDirectBufferCapacity(void *env, void *buf) {
  (void)env; if (buf == &g_fmod_bb_sentinel) return (long)g_fmod_cap; return -1;
}
static void *jni_NewObject(void *env, void *clazz, void *mid, ...) {
  (void)env; (void)mid;
  if (clazz == class_for("org/fmod/FMODAudioDevice")) {
    debugPrintf("jni_shim: NewObject(FMODAudioDevice) -> device fake\n"); return &g_fmod_device_obj;
  }
  return NULL;   /* comportamento atual (jni_stub=0) p/ as demais classes — sem regressao */
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
  jni_env_vtable[55] = (uintptr_t)jni_CallFloatMethod;     /* getAxisValue (gamepad analogico) */
  jni_env_vtable[56] = (uintptr_t)jni_CallFloatMethodV;    /* V (va_list) */
  jni_env_vtable[34] = (uintptr_t)jni_CallObjectMethod;
  jni_env_vtable[35] = (uintptr_t)jni_CallObjectMethodV;   /* V variant (va_list) */
  jni_env_vtable[37] = (uintptr_t)jni_CallBooleanMethod;
  jni_env_vtable[38] = (uintptr_t)jni_CallBooleanMethod;   /* V */
  jni_env_vtable[49] = (uintptr_t)jni_CallIntMethod;
  jni_env_vtable[50] = (uintptr_t)jni_CallIntMethodV;      /* V (va_list) */
  jni_env_vtable[61] = (uintptr_t)jni_CallVoidMethod;
  jni_env_vtable[62] = (uintptr_t)jni_CallVoidMethod;      /* V */
  jni_env_vtable[94] = (uintptr_t)jni_GetFieldID;
  jni_env_vtable[113] = (uintptr_t)jni_GetStaticMethodID;
  jni_env_vtable[114] = (uintptr_t)jni_CallStaticObjectMethod;
  jni_env_vtable[115] = (uintptr_t)jni_CallStaticObjectMethodV; /* V (va_list) — encode/decode */
  jni_env_vtable[117] = (uintptr_t)jni_CallStaticBooleanMethod;
  jni_env_vtable[118] = (uintptr_t)jni_CallStaticBooleanMethod; /* V */
  jni_env_vtable[129] = (uintptr_t)jni_CallStaticIntMethod;
  jni_env_vtable[130] = (uintptr_t)jni_CallStaticIntMethod; /* V */
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
  jni_env_vtable[187] = (uintptr_t)jni_GetIntArrayElements;   /* getDeviceIds int[] */
  jni_env_vtable[195] = (uintptr_t)jni_ReleaseIntArrayElements;
  jni_env_vtable[203] = (uintptr_t)jni_GetIntArrayRegion;
  jni_env_vtable[205] = (uintptr_t)jni_ExceptionCheck;
  jni_env_vtable[230] = (uintptr_t)jni_GetDirectBufferAddress;  /* FMOD fmodProcess PCM */
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
