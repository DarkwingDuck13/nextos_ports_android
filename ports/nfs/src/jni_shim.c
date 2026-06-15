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
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "jni_shim.h"
#include "util.h"

/* ---- font_shim (rasterização de texto) e bitmap (imports.c) ---- */
extern void *font_create_from_file(const char *path, float size);
extern void *font_create_from_family(const char *fontsdir, const char *family, float size);
extern void font_set_size(void *paint, float size);
extern float font_get_size(void *paint);
extern float font_measure(void *paint, const char *str);
extern void font_draw(void *paint, const char *str, int x, int y,
                      unsigned char *buf, int bw, int bh, int stride);
extern unsigned char *nfs_abm_buf(void);
extern int nfs_abm_w(void); extern int nfs_abm_h(void); extern int nfs_abm_stride(void);

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
  MID_GET_OBB_PATH,
  MID_GET_ABS_PATH,
  MID_GET_FILES_DIR,
  MID_GET_PACK_NAME,
  MID_SET_ACTIVITY,
  MID_ERROR_DIALOG,
  MID_GET_CLASS_LOADER,
  MID_LOAD_CLASS,
  MID_IS_OBB,
  MID_USE_ASSETS_FS,
  MID_IS_FULL_APK,
  MID_GET_ASSET_SIZE,
  MID_APP_VERSION,
  MID_DEVICE_NAME,
  MID_LOCALE,
  MID_LANGUAGE,
  MID_OS_VERSION,
  MID_GET_BITMAP,
  MID_GET_WIDTH,
  MID_GET_HEIGHT,
  MID_GET_TOTAL_MEMORY,
  MID_GET_PERF_SCORE,
  /* fonte/texto (BitmapGraphics/Paint) */
  MID_CREATE_PAINT_FILE,
  MID_CREATE_PAINT_FAMILY,
  MID_SET_TEXT_SIZE,
  MID_GET_TEXT_SIZE,
  MID_GET_FONT_METRICS,
  MID_MEASURE_TEXT,
  MID_DRAW_STRING,
  MID_BMG_CLEAR,
  MID_GET_KEYCODE,   /* KeyEvent.getKeyCode() — gamepad/Moga */
  MID_GET_ACTION,    /* KeyEvent.getAction() */
  MID_GET_STATUS,    /* INetwork.getStatus() -> Network$Status (conectividade) */
  MID_ORDINAL,       /* Enum.ordinal() — usado p/ Network$Status */
  MID_GET_AXIS,      /* MotionEvent.getAxisValue(int) — analog stick (Moga) */
  MID_GENERIC,
  FID_OBB_VERSIONCODE,
  FID_WIDTH,
  FID_HEIGHT,
  FID_DENSITY,
  FID_DENSITY_DPI,
  FID_FM_ASCENT,
  FID_FM_DESCENT,
  FID_FM_TOP,
  FID_FM_BOTTOM,
  FID_FM_LEADING,
  FID_GENERIC,
};

static int g_method_tags[64]; /* unique addresses used as method IDs */

/* 🎮 injeção de gamepad (Moga): main.c seta estes antes de chamar
 * MogaController_nativeOnKeyEvent. A engine lê KeyEvent.getKeyCode()/getAction()
 * via CallIntMethodV — durante a janela g_moga_active retornamos o valor. */
int g_moga_active = 0;
int g_moga_keycode = 0;
int g_moga_action = 0;
int g_moga_calln = 0;  /* main.c zera antes de cada nativeOnKeyEvent */
/* 🕹️ injeção de analog stick (MotionEvent): main.c seta os eixos antes de chamar
 * nativeOnMotionEvent. getAxisValue(axis) via CallFloatMethodV devolve g_axis[axis]. */
int g_motion_active = 0;
float g_axis[32] = {0};

/* ---- Configurable package/OBB ---- */
static const char *g_package_name = "com.microids.syberia";
static int g_obb_version = 12;

void jni_shim_set_package(const char *package_name, int obb_version) {
  g_package_name = package_name;
  g_obb_version = obb_version;
}

/* ---- Fake jstring tracking ---- */
/* We return tagged pointers as jstrings and map them to C strings */
#define MAX_JSTRINGS 512
static struct {
  void *handle;
  char *value; /* CÓPIA própria (não ponteiro do buffer da engine, que é reusado) */
} g_jstrings[MAX_JSTRINGS];
static int g_jstring_count = 0;

/* 🔑 COPIA o conteúdo: NewStringUTF recebe um buffer da engine que é reusado/
 * liberado; guardar o ponteiro fazia o texto virar LIXO no drawString. */
static void *make_jstring(const char *value) {
  static char jstring_storage[MAX_JSTRINGS];
  if (g_jstring_count >= MAX_JSTRINGS)
    g_jstring_count = 0; /* wrap around */
  int idx = g_jstring_count++;
  free(g_jstrings[idx].value);
  g_jstrings[idx].value = strdup(value ? value : "");
  g_jstrings[idx].handle = &jstring_storage[idx];
  return g_jstrings[idx].handle;
}

static const char *resolve_jstring(void *jstr) {
  for (int i = 0; i < g_jstring_count; i++) {
    if (g_jstrings[i].handle == jstr)
      return g_jstrings[i].value;
  }
  return "";
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
  static int fake_class;
  return &fake_class;
}

/* config strings (versão/device/locale): retornar VAZIO faz a engine tokenizar
 * "" e criar objeto garbage → assert. Damos valores válidos. */
static void *jni_match_config(const char *name) {
  if (strstr(name, "ApplicationVersion") || strstr(name, "VersionName"))
    return &g_method_tags[MID_APP_VERSION];
  if (strstr(name, "DeviceName")) return &g_method_tags[MID_DEVICE_NAME];
  if (strstr(name, "Locale")) return &g_method_tags[MID_LOCALE];
  if (strstr(name, "Language")) return &g_method_tags[MID_LANGUAGE];
  if (strstr(name, "OsVersion") || strstr(name, "OSVersion")) return &g_method_tags[MID_OS_VERSION];
  return 0;
}

static void *jni_GetMethodID(void *env, void *clazz, const char *name,
                             const char *sig) {
  (void)env;
  (void)clazz;
  debugPrintf("jni_shim: GetMethodID(%s, %s)\n", name, sig);
  if (strcmp(name, "getClassLoader") == 0)
    return &g_method_tags[MID_GET_CLASS_LOADER];
  if (strcmp(name, "loadClass") == 0)
    return &g_method_tags[MID_LOAD_CLASS];
  /* NFS: cadeia de path storage/OBB. getExternalStorageDirectory()/getFilesDir()
   * retornam um File; getAbsolutePath() nele → a String do path. */
  if (strstr(name, "ObbFullPath") || strstr(name, "ObbPath"))
    return &g_method_tags[MID_GET_OBB_PATH];
  if (strstr(name, "getAbsolutePath") || strstr(name, "getPath") || strstr(name, "getCanonicalPath"))
    return &g_method_tags[MID_GET_ABS_PATH];
  if (strstr(name, "getFilesDir"))
    return &g_method_tags[MID_GET_FILES_DIR];
  if (strcmp(name, "getPackageName") == 0 || strcmp(name, "getPackName") == 0)
    return &g_method_tags[MID_GET_PACK_NAME];
  if (strcmp(name, "getBitmap") == 0) return &g_method_tags[MID_GET_BITMAP];
  if (strcmp(name, "getWidth") == 0) return &g_method_tags[MID_GET_WIDTH];
  if (strcmp(name, "getHeight") == 0) return &g_method_tags[MID_GET_HEIGHT];
  /* 🔑 getTotalMemory()I=0 → orçamento de cache de textura=0 → engine despeja
   * TODO atlas logo após carregar (Add→Remove) → sprites "not found" → zero draws.
   * Reportar memória real (MB) mantém os texturepacks residentes. */
  if (strcmp(name, "getTotalMemory") == 0) return &g_method_tags[MID_GET_TOTAL_MEMORY];
  if (strcmp(name, "getPerformanceScore") == 0) return &g_method_tags[MID_GET_PERF_SCORE];
  /* fonte/texto (BitmapGraphics/Paint) */
  if (strcmp(name, "setTextSize") == 0) return &g_method_tags[MID_SET_TEXT_SIZE];
  if (strcmp(name, "getTextSize") == 0) return &g_method_tags[MID_GET_TEXT_SIZE];
  if (strcmp(name, "getFontMetricsInt") == 0) return &g_method_tags[MID_GET_FONT_METRICS];
  if (strcmp(name, "measureText") == 0) return &g_method_tags[MID_MEASURE_TEXT];
  if (strcmp(name, "drawString") == 0) return &g_method_tags[MID_DRAW_STRING];
  if (strcmp(name, "clear") == 0) return &g_method_tags[MID_BMG_CLEAR];
  if (strcmp(name, "getKeyCode") == 0) return &g_method_tags[MID_GET_KEYCODE];
  if (strcmp(name, "getAction") == 0) return &g_method_tags[MID_GET_ACTION];
  if (strcmp(name, "getStatus") == 0) return &g_method_tags[MID_GET_STATUS];
  if (strcmp(name, "ordinal") == 0) return &g_method_tags[MID_ORDINAL];
  if (strcmp(name, "getAxisValue") == 0) return &g_method_tags[MID_GET_AXIS];
  if (strcmp(name, "isObbAssets") == 0) return &g_method_tags[MID_IS_OBB];
  if (strcmp(name, "useAssetsFileSystem") == 0) return &g_method_tags[MID_USE_ASSETS_FS];
  if (strcmp(name, "isFullApkAssets") == 0) return &g_method_tags[MID_IS_FULL_APK];
  if (strcmp(name, "getAssetSize") == 0) return &g_method_tags[MID_GET_ASSET_SIZE];
  void *cfg = jni_match_config(name);
  if (cfg) return cfg;
  if (strstr(name, "ExternalStorageDirectory") || strstr(name, "StorageDirectory") ||
      strstr(name, "StorageDir"))
    return &g_method_tags[MID_GET_STORAGE_DIR];
  return &g_method_tags[MID_GENERIC];
}

static void *jni_GetStaticMethodID(void *env, void *clazz, const char *name,
                                   const char *sig) {
  (void)env;
  (void)clazz;
  debugPrintf("jni_shim: GetStaticMethodID(%s, %s)\n", name, sig);
  { void *cfg = jni_match_config(name); if (cfg) return cfg; }
  if (strcmp(name, "createPaintFromFile") == 0) return &g_method_tags[MID_CREATE_PAINT_FILE];
  if (strcmp(name, "createPaintFromFamilyName") == 0) return &g_method_tags[MID_CREATE_PAINT_FAMILY];
  if (strcmp(name, "getStorageDir") == 0)
    return &g_method_tags[MID_GET_STORAGE_DIR];
  if (strstr(name, "ObbFullPath") || strstr(name, "ObbPath"))
    return &g_method_tags[MID_GET_OBB_PATH];
  if (strstr(name, "ExternalStorageDirectory") || strstr(name, "StorageDirectory"))
    return &g_method_tags[MID_GET_STORAGE_DIR];
  if (strcmp(name, "getPackName") == 0)
    return &g_method_tags[MID_GET_PACK_NAME];
  if (strcmp(name, "setActivity") == 0)
    return &g_method_tags[MID_SET_ACTIVITY];
  if (strcmp(name, "errorDialog") == 0)
    return &g_method_tags[MID_ERROR_DIALOG];
  return &g_method_tags[MID_GENERIC];
}

static void *jni_GetFieldID(void *env, void *clazz, const char *name,
                            const char *sig) {
  (void)env;
  (void)clazz;
  debugPrintf("jni_shim: GetFieldID(%s, %s)\n", name, sig);
  if (strcmp(name, "widthPixels") == 0) return &g_method_tags[FID_WIDTH];
  if (strcmp(name, "heightPixels") == 0) return &g_method_tags[FID_HEIGHT];
  if (strcmp(name, "density") == 0) return &g_method_tags[FID_DENSITY];
  if (strcmp(name, "densityDpi") == 0) return &g_method_tags[FID_DENSITY_DPI];
  /* campos do Paint$FontMetricsInt (ints): ascent/descent/top/bottom/leading */
  if (strcmp(name, "ascent") == 0) return &g_method_tags[FID_FM_ASCENT];
  if (strcmp(name, "descent") == 0) return &g_method_tags[FID_FM_DESCENT];
  if (strcmp(name, "top") == 0) return &g_method_tags[FID_FM_TOP];
  if (strcmp(name, "bottom") == 0) return &g_method_tags[FID_FM_BOTTOM];
  if (strcmp(name, "leading") == 0) return &g_method_tags[FID_FM_LEADING];
  return &g_method_tags[FID_GENERIC];
}

/* métricas da fonte correntes (setadas por getFontMetricsInt) p/ os GetIntField */
extern void font_metrics(void *paint, int *a, int *d, int *t, int *b, int *l);
static void *g_fm_paint; /* Paint cujo getFontMetricsInt foi chamado por último */

/* GetIntField/GetFloatField: tela 1280x720, densidade 2.0/320dpi */
static jint jni_GetIntField(void *env, void *obj, void *fid) {
  (void)env; (void)obj;
  if (fid == &g_method_tags[FID_WIDTH]) return 1280;
  if (fid == &g_method_tags[FID_HEIGHT]) return 720;
  if (fid == &g_method_tags[FID_DENSITY_DPI]) return 320;
  if (fid == &g_method_tags[FID_FM_ASCENT] || fid == &g_method_tags[FID_FM_DESCENT] ||
      fid == &g_method_tags[FID_FM_TOP] || fid == &g_method_tags[FID_FM_BOTTOM] ||
      fid == &g_method_tags[FID_FM_LEADING]) {
    int a, d, t, b, l; font_metrics(g_fm_paint, &a, &d, &t, &b, &l);
    if (fid == &g_method_tags[FID_FM_ASCENT]) return a;
    if (fid == &g_method_tags[FID_FM_DESCENT]) return d;
    if (fid == &g_method_tags[FID_FM_TOP]) return t;
    if (fid == &g_method_tags[FID_FM_BOTTOM]) return b;
    return l;
  }
  return 0;
}
/* 🔑 A engine é SOFTFP (float de retorno em r0); nosso loader é HARDFP (float em
 * s0/VFP). Sem pcs("aapcs"), a engine lia o float de r0 = LIXO (ScreenDensity =
 * 2.73315e-40) → layout/view scaling quebrado → "Primary view size: 0 0" →
 * TUDO culled → zero draws. pcs("aapcs") faz retornar o float em r0 (softfp). */
__attribute__((pcs("aapcs")))
static float jni_GetFloatField(void *env, void *obj, void *fid) {
  (void)env; (void)obj;
  if (fid == &g_method_tags[FID_DENSITY]) return 2.0f;
  return 0.0f;
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

/* path base dos dados (env NFS_DATA) + path completo do OBB */
static const char *nfs_data_dir(void) {
  const char *d = getenv("NFS_DATA");
  return (d && *d) ? d : "/storage/roms/nfs/data";
}
static const char *nfs_obb_path(void) {
  static char p[512];
  snprintf(p, sizeof(p), "%s/Android/obb/%s/main.%d.%s.obb",
           nfs_data_dir(), g_package_name, 1003128, g_package_name);
  return p;
}

/* retorna o jstring da config (ou NULL se methodID não é config) */
static void *jni_config_jstr(void *methodID) {
  if (methodID == &g_method_tags[MID_APP_VERSION]) return make_jstring("1.3.128");
  if (methodID == &g_method_tags[MID_DEVICE_NAME]) return make_jstring("NextOS");
  if (methodID == &g_method_tags[MID_LOCALE]) return make_jstring("en_US");
  if (methodID == &g_method_tags[MID_LANGUAGE]) return make_jstring("en");
  if (methodID == &g_method_tags[MID_OS_VERSION]) return make_jstring("9");
  return 0;
}

/* CallObjectMethod (index 36) - variadic */
/* sentinel devolvido por INetwork.getStatus(); ordinal() o reconhece p/ devolver
 * o status de rede "conectado" (sem afetar ordinal() de outros enums). */
static int g_net_status_sentinel;
static void *jni_CallObjectMethod(void *env, void *obj, void *methodID, ...) {
  (void)env;
  (void)obj;
  { void *cv = jni_config_jstr(methodID); if (cv) return cv; }
  /* 🌐 INetwork.getStatus() → Network$Status. Devolvemos um sentinel não-null
   * cujo ordinal() = status conectado → o flow do EULA não roteia p/
   * NO_CONNECTION_PROMPT (beco sem saída offline) e prossegue. */
  if (methodID == &g_method_tags[MID_GET_STATUS]) return &g_net_status_sentinel;
  if (methodID == &g_method_tags[MID_GET_STORAGE_DIR]) {
    debugPrintf("jni_shim: CallObjectMethod -> storageDir = %s\n", nfs_data_dir());
    return make_jstring(nfs_data_dir());
  }
  if (methodID == &g_method_tags[MID_GET_OBB_PATH]) {
    debugPrintf("jni_shim: CallObjectMethod -> obbPath = %s\n", nfs_obb_path());
    return make_jstring(nfs_obb_path());
  }
  if (methodID == &g_method_tags[MID_GET_FILES_DIR]) {
    static char fd[512]; snprintf(fd, sizeof(fd), "%s/files", nfs_data_dir());
    debugPrintf("jni_shim: CallObjectMethod -> filesDir = %s\n", fd);
    return make_jstring(fd);
  }
  if (methodID == &g_method_tags[MID_GET_ABS_PATH]) {
    /* getAbsolutePath() do File: o 'obj' (File) já é nosso jstring do path */
    debugPrintf("jni_shim: CallObjectMethod -> getAbsolutePath = %s\n", resolve_jstring(obj));
    return obj;
  }
  if (methodID == &g_method_tags[MID_GET_PACK_NAME]) {
    debugPrintf("jni_shim: CallObjectMethod -> packageName = %s\n", g_package_name);
    return make_jstring(g_package_name);
  }
  if (methodID == &g_method_tags[MID_GET_BITMAP]) {
    static int bitmap_obj; /* handle distinto p/ o Bitmap (abm_* ignoram o ptr) */
    if (getenv("NFS_BMPLOG")) fprintf(stderr, "[jni getBitmap] -> %p\n", (void *)&bitmap_obj);
    return &bitmap_obj;
  }
  if (methodID == &g_method_tags[MID_GET_FONT_METRICS]) {
    /* getFontMetricsInt() do Paint (obj). Lembra o paint p/ os GetIntField
     * (ascent/descent/...) e devolve um handle do FontMetricsInt. */
    static int fm_obj;
    g_fm_paint = obj;
    return &fm_obj;
  }
  debugPrintf("jni_shim: CallObjectMethod(mid=%p)\n", methodID);
  static int fake_obj;
  return &fake_obj;
}

/* CallBooleanMethod (index 49) */
static unsigned char jni_CallBooleanMethod(void *env, void *obj,
                                           void *methodID, ...) {
  (void)env;
  (void)obj;
  if (methodID == &g_method_tags[MID_IS_OBB]) {
    /* PADRÃO=0 (disco): a engine abre o OBB como ZIP via "stream" e FALHA
     * ("Error opening ZIP stream", AssetsFileSystem com métodos não implementados);
     * lendo do disco (published/+published.1x/ extraídos) os texturepacks .sba
     * carregam. NFS_USEOBB=1 reativa o caminho do OBB (debug). */
    int v = getenv("NFS_USEOBB") ? 1 : 0;
    debugPrintf("jni_shim: isObbAssets -> %d\n", v);
    return v; /* assets no OBB (1) ou no disco (0) */
  }
  if (methodID == &g_method_tags[MID_USE_ASSETS_FS]) {
    int v = getenv("NFS_USEASSETSFS") ? 1 : 0;  /* experimento: montar base published/ */
    debugPrintf("jni_shim: useAssetsFileSystem -> %d\n", v);
    return v;
  }
  return 0; /* isFullApkAssets -> 0 */
}

/* CallIntMethod (index 61) — getWidth/getHeight NÃO são forçados (há chamadas
 * precoces não-Bitmap que esperam 0); o caminho do Bitmap usa abm_getInfo. */
static jint jni_CallIntMethod(void *env, void *obj, void *methodID, ...) {
  (void)env;
  (void)obj;
  /* 🎮 durante a injeção de gamepad, KeyEvent.getKeyCode()/getAction() do
   * nativeOnKeyEvent caem aqui (CallIntMethodV slot 50). Respondemos com o
   * keycode/ação injetados. Cobrimos tanto o methodID cacheado via nosso
   * GetMethodID quanto o caso de methodID não-resolvido (g_moga_active). */
  if (g_moga_active) {
    /* nativeOnKeyEvent lê, NESTA ordem: getKeyCode (log), getKeyCode (switch),
     * getAction (down/up). Os methodIDs do KeyEvent NÃO são cacheados via nosso
     * GetMethodID (não há Java MogaController) → não dá p/ distinguir por
     * methodID. Usamos o contador: 1ª/2ª chamada=keycode, 3ª+=action. action
     * DEVE ser 0(DOWN)/1(UP) senão a engine bail. */
    if (methodID == &g_method_tags[MID_GET_ACTION]) return g_moga_action;
    if (methodID == &g_method_tags[MID_GET_KEYCODE]) return g_moga_keycode;
    int n = g_moga_calln++;
    return (n < 2) ? g_moga_keycode : g_moga_action;
  }
  if (methodID == &g_method_tags[MID_GET_KEYCODE]) return g_moga_keycode;
  if (methodID == &g_method_tags[MID_GET_ACTION]) return g_moga_action;
  /* 🌐 Network$Status.ordinal(): só p/ o nosso sentinel (não mexe noutros enums).
   * Nimble Network.Status: 0=UNKNOWN/sem-conexão. NFS_NETSTATUS (default 1=NORMAL)
   * = conectado → EULA prossegue. */
  if (methodID == &g_method_tags[MID_ORDINAL]) {
    if (obj == &g_net_status_sentinel) {
      /* Nimble Network.Status: ordinal 3 = conectado p/ este build (validado:
       * 3 não dispara NO_CONNECTION_PROMPT). NFS_NETSTATUS sobrescreve. */
      if (getenv("NFS_STACKSCAN")) {
        extern void *text_base; uintptr_t tb=(uintptr_t)text_base;
        void *ra[6]={0};
        ra[0]=__builtin_return_address(0);
        ra[1]=__builtin_return_address(1);
        ra[2]=__builtin_return_address(2);
        ra[3]=__builtin_return_address(3);
        ra[4]=__builtin_return_address(4);
        fprintf(stderr, "[RACHAIN]");
        for(int i=0;i<5;i++){ uintptr_t v=(uintptr_t)ra[i];
          if(v>tb && v<tb+0xa00000) fprintf(stderr," +0x%lx",(unsigned long)(v-tb)); else fprintf(stderr," (%p)",ra[i]); }
        fprintf(stderr,"\n");
      }
      const char *e = getenv("NFS_NETSTATUS");
      return e ? atoi(e) : 3;
    }
    return 0;  /* ordinal() de outros enums: comportamento default inalterado */
  }
  if (methodID == &g_method_tags[MID_GET_TOTAL_MEMORY]) {
    int mb = getenv("NFS_MEMMB") ? atoi(getenv("NFS_MEMMB")) : 2048;
    debugPrintf("jni_shim: getTotalMemory -> %d MB\n", mb);
    return mb;
  }
  /* getWidth/getHeight do GameGLSurfaceView: o renderer tira o tamanho da view
   * daqui. 0 → "Primary view size: 0 0" → glViewport 0x0 → tudo culled → zero
   * draws. NFS_WH=1 força 1280/720. (O caminho do Bitmap usa AndroidBitmap_getInfo,
   * não getWidth/getHeight JNI, então não deve colidir.) */
  if (!getenv("NFS_NOWH")) {
    if (methodID == &g_method_tags[MID_GET_WIDTH]) return 1280;
    if (methodID == &g_method_tags[MID_GET_HEIGHT]) return 720;
  }
  return 0;
}

/* CallFloatMethod (index 55) — getPerformanceScore()F. 0 força Tier=Low (ok p/
 * Mali-450). Mantém 0 por padrão; NFS_PERF=N permite forçar score maior. */
__attribute__((pcs("aapcs")))
static float jni_CallFloatMethod_v(void *env, void *obj, void *methodID, va_list ap) {
  (void)env; (void)obj;
  /* 🕹️ MotionEvent.getAxisValue(axis) — durante injeção de stick devolve o eixo
   * setado. O methodID de getAxisValue pode NÃO estar cacheado via nosso
   * GetMethodID (sem Java MogaController), então durante g_motion_active tratamos
   * QUALQUER CallFloat como getAxisValue (na janela, nativeOnMotionEvent só chama
   * getAxisValue) e lemos o axis do vararg. */
  if (g_motion_active || methodID == &g_method_tags[MID_GET_AXIS]) {
    int axis = va_arg(ap, int);
    if (axis >= 0 && axis < 32) return g_axis[axis];
    return 0.0f;
  }
  if (methodID == &g_method_tags[MID_GET_PERF_SCORE]) {
    float s = getenv("NFS_PERF") ? (float)atof(getenv("NFS_PERF")) : 0.0f;
    debugPrintf("jni_shim: getPerformanceScore -> %f\n", s);
    return s;
  }
  if (methodID == &g_method_tags[MID_GET_TEXT_SIZE])
    return font_get_size(obj);
  if (methodID == &g_method_tags[MID_MEASURE_TEXT]) {
    void *jstr = va_arg(ap, void *);
    return font_measure(obj, resolve_jstring(jstr));
  }
  return 0.0f;
}
__attribute__((pcs("aapcs")))
static float jni_CallFloatMethod(void *env, void *obj, void *methodID, ...) {
  va_list ap; va_start(ap, methodID);
  float r = jni_CallFloatMethod_v(env, obj, methodID, ap);
  va_end(ap);
  return r;
}
__attribute__((pcs("aapcs")))
static float jni_CallFloatMethodV(void *env, void *obj, void *methodID, va_list ap) {
  return jni_CallFloatMethod_v(env, obj, methodID, ap);
}

/* core: recebe um va_list (compartilhado entre CallVoidMethod e ...V) */
static void jni_CallVoidMethod_v(void *env, void *obj, void *methodID, va_list ap) {
  (void)env;
  if (methodID == &g_method_tags[MID_SET_TEXT_SIZE]) {
    double sz = va_arg(ap, double); /* float promovido a double em varargs */
    font_set_size(obj, (float)sz);
    return;
  }
  if (methodID == &g_method_tags[MID_BMG_CLEAR]) {
    /* BitmapGraphics.clear() → zera o bitmap antes de desenhar texto */
    unsigned char *b = nfs_abm_buf();
    if (b) memset(b, 0, (size_t)nfs_abm_h() * nfs_abm_stride());
    return;
  }
  if (methodID == &g_method_tags[MID_DRAW_STRING]) {
    /* drawString(Paint, String, int x, int y) — obj = BitmapGraphics */
    void *paint = va_arg(ap, void *);
    void *jstr = va_arg(ap, void *);
    int x = va_arg(ap, int);
    int y = va_arg(ap, int);
    const char *s = resolve_jstring(jstr);
    if (getenv("NFS_FONTLOG"))
      fprintf(stderr, "[drawString] paint=%p x=%d y=%d '%s'\n", paint, x, y, s ? s : "");
    font_draw(paint, s, x, y, nfs_abm_buf(), nfs_abm_w(), nfs_abm_h(), nfs_abm_stride());
    return;
  }
  (void)obj;
}
/* CallVoidMethod (index 61) — varargs */
static void jni_CallVoidMethod(void *env, void *obj, void *methodID, ...) {
  va_list ap; va_start(ap, methodID);
  jni_CallVoidMethod_v(env, obj, methodID, ap);
  va_end(ap);
}
/* CallVoidMethodV (index 62) — recebe va_list (a engine usa esta p/ drawString!) */
static void jni_CallVoidMethodV(void *env, void *obj, void *methodID, va_list ap) {
  jni_CallVoidMethod_v(env, obj, methodID, ap);
}

/* CallStaticObjectMethod core (va_list) */
static void *jni_CallStaticObjectMethod_v(void *env, void *clazz,
                                          void *methodID, va_list ap) {
  (void)env;
  (void)clazz;
  { void *cv = jni_config_jstr(methodID); if (cv) return cv; }

  if (methodID == &g_method_tags[MID_GET_STORAGE_DIR]) {
    debugPrintf("jni_shim: CallStaticObjectMethod -> storageDir = %s\n", nfs_data_dir());
    return make_jstring(nfs_data_dir());
  }
  if (methodID == &g_method_tags[MID_GET_OBB_PATH]) {
    debugPrintf("jni_shim: CallStaticObjectMethod -> obbPath = %s\n", nfs_obb_path());
    return make_jstring(nfs_obb_path());
  }
  if (methodID == &g_method_tags[MID_GET_PACK_NAME]) {
    debugPrintf(
        "jni_shim: CallStaticObjectMethod -> getPackName = \"%s\"\n",
        g_package_name);
    return make_jstring(g_package_name);
  }
  static int fake_result;
  if (methodID == &g_method_tags[MID_CREATE_PAINT_FILE] ||
      methodID == &g_method_tags[MID_CREATE_PAINT_FAMILY]) {
    /* createPaintFromFile(String path, float size) / FromFamilyName(String, float) */
    void *jstr = va_arg(ap, void *);
    double size = va_arg(ap, double); /* float promovido a double em varargs */
    const char *s = resolve_jstring(jstr);
    void *paint;
    if (methodID == &g_method_tags[MID_CREATE_PAINT_FILE]) {
      paint = font_create_from_file(s, (float)size);
    } else {
      static char fdir[600];
      snprintf(fdir, sizeof fdir, "%s/files/published/fonts", nfs_data_dir());
      paint = font_create_from_family(fdir, s, (float)size);
    }
    return paint ? paint : &fake_result;
  }

  debugPrintf("jni_shim: CallStaticObjectMethod(mid=%p) -> NULL\n", methodID);
  return &fake_result;
}
/* CallStaticObjectMethod (index 113) — varargs */
static void *jni_CallStaticObjectMethod(void *env, void *clazz, void *methodID, ...) {
  va_list ap; va_start(ap, methodID);
  void *r = jni_CallStaticObjectMethod_v(env, clazz, methodID, ap);
  va_end(ap);
  return r;
}
/* CallStaticObjectMethodV (index 114) — va_list */
static void *jni_CallStaticObjectMethodV(void *env, void *clazz, void *methodID, va_list ap) {
  return jni_CallStaticObjectMethod_v(env, clazz, methodID, ap);
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
/* 🔑 IsSameObject (slot 24): o dispatch de TOQUE (nativeTouchScreenEvent ->
 * 0x54a284) faz IsSameObject(env, handler->view, thiz) p/ casar o handler
 * registrado com a view do evento. Sem isto caía no jni_stub (=0) → match
 * SEMPRE falhava → todo toque era descartado. Compara ponteiros de jobject. */
static unsigned char jni_IsSameObject(void *env, void *a, void *b) {
  (void)env;
  return (a == b) ? 1 : 0;
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
  (void)obj;
  static int fake_obj_class;
  return &fake_obj_class;
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
  (void)array;
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
  jni_env_vtable[17] = (uintptr_t)jni_ExceptionClear;
  jni_env_vtable[21] = (uintptr_t)jni_NewGlobalRef;
  jni_env_vtable[22] = (uintptr_t)jni_DeleteGlobalRef;
  jni_env_vtable[23] = (uintptr_t)jni_DeleteLocalRef;
  jni_env_vtable[24] = (uintptr_t)jni_IsSameObject;       /* match de view no toque */
  jni_env_vtable[25] = (uintptr_t)jni_NewLocalRef;
  jni_env_vtable[31] = (uintptr_t)jni_GetObjectClass;
  jni_env_vtable[33] = (uintptr_t)jni_GetMethodID;
  jni_env_vtable[34] = (uintptr_t)jni_CallObjectMethod;
  jni_env_vtable[35] = (uintptr_t)jni_CallObjectMethod;    /* V variant */
  jni_env_vtable[37] = (uintptr_t)jni_CallBooleanMethod;
  jni_env_vtable[38] = (uintptr_t)jni_CallBooleanMethod;   /* V */
  jni_env_vtable[49] = (uintptr_t)jni_CallIntMethod;
  jni_env_vtable[50] = (uintptr_t)jni_CallIntMethod;       /* V */
  jni_env_vtable[55] = (uintptr_t)jni_CallFloatMethod;
  jni_env_vtable[56] = (uintptr_t)jni_CallFloatMethodV;    /* V (va_list) */
  jni_env_vtable[61] = (uintptr_t)jni_CallVoidMethod;
  jni_env_vtable[62] = (uintptr_t)jni_CallVoidMethodV;     /* V (va_list) - drawString */
  jni_env_vtable[94] = (uintptr_t)jni_GetFieldID;
  jni_env_vtable[100] = (uintptr_t)jni_GetIntField;
  jni_env_vtable[102] = (uintptr_t)jni_GetFloatField;
  jni_env_vtable[113] = (uintptr_t)jni_GetStaticMethodID;
  jni_env_vtable[114] = (uintptr_t)jni_CallStaticObjectMethod;
  jni_env_vtable[115] = (uintptr_t)jni_CallStaticObjectMethodV; /* V (va_list) - createPaint */
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
  jni_env_vtable[205] = (uintptr_t)jni_ExceptionCheck;

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
