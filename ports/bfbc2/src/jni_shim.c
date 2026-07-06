/*
 * jni_shim.c — fake JNI para o Karisma engine (BFBC2).
 *
 * Estratégia: GetMethodID/GetFieldID devolvem o PONTEIRO do nome (string
 * estática do engine, estável) como ID; os Call e Get*Field despacham por
 * strcmp(name). Objetos (RawResource, FileDescriptor, jstring, short[]) são
 * structs com tag, alocadas por nós. Índices da vtable = JNI 1.6 padrão.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include "jni_shim.h"
#include "util.h"

#define VT 260

static uintptr_t env_vt[VT];
static uintptr_t vm_vt[VT];
static void *env_ptr = env_vt;
static void *vm_ptr  = vm_vt;

volatile int g_snd_enabled = 0;
volatile int g_snd_locked = 0;
volatile int g_want_exit = 0;

static int g_scr_w = 1280, g_scr_h = 720;
static char g_assets_dir[512] = ".";

void jni_shim_set_screen(int w, int h) { g_scr_w = w; g_scr_h = h; }
void jni_shim_set_assets_dir(const char *d) { snprintf(g_assets_dir, sizeof g_assets_dir, "%s", d); }

static int g_verbose = -1;
static int verbose(void) { if (g_verbose < 0) g_verbose = getenv("BC2_JNILOG") ? 1 : 0; return g_verbose; }

/* ---------------- objetos fake ---------------- */
enum { T_STR = 1, T_RAWRES, T_FD, T_SHORTARR, T_CLASS, T_OBJ };
typedef struct fake_obj {
  int tag;
  /* string */
  char *s;
  /* rawresource */
  int status; long len; long off; struct fake_obj *jfd;
  /* fd */
  int fd;
  /* short array */
  short *data; int alen;
  /* class */
  const char *cname;
} fake_obj;

static fake_obj *new_obj(int tag) { fake_obj *o = calloc(1, sizeof *o); o->tag = tag; return o; }
static fake_obj *mk_str(const char *s) { fake_obj *o = new_obj(T_STR); o->s = s ? strdup(s) : NULL; return o; }

void *jni_make_short_array(short *data, int len) {
  fake_obj *o = new_obj(T_SHORTARR); o->data = data; o->alen = len; return o;
}

/* classes conhecidas (singletons por nome) */
static fake_obj cls_karisma = { .tag = T_CLASS, .cname = "com/dle/bc2/KarismaBridge" };
static fake_obj cls_rawres  = { .tag = T_CLASS, .cname = "com/dle/bc2/RawResource" };
static fake_obj cls_fd      = { .tag = T_CLASS, .cname = "java/io/FileDescriptor" };
static fake_obj cls_generic = { .tag = T_CLASS, .cname = "?" };

/* ---------------- helpers ---------------- */
static const char *as_cstr(void *jstr) {
  fake_obj *o = jstr;
  if (o && o->tag == T_STR) return o->s;
  return NULL;
}

/* abre o asset cujo nome começa por 'prefix' na pasta de assets */
static int open_asset_prefix(const char *prefix, long *out_len) {
  /* os assets foram extraídos com o basename original (sem o "assets/").
   * O engine passa prefixos tipo "multilang/en" ou "downloadcontent/config".
   * Procuramos <assets_dir>/<prefix>* — casamos por prefixo do caminho. */
  char path[1024];
  snprintf(path, sizeof path, "%s/%s", g_assets_dir, prefix);
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    /* tentativa com glob simples: <dir>/<prefix> exato falhou; procura arquivo
     * que comece pelo último componente. Deixa pro fopen-redirect no imports. */
    if (verbose()) debugPrintf("[jni] OpenResource('%s') -> nao achou %s\n", prefix, path);
    return -1;
  }
  off_t sz = lseek(fd, 0, SEEK_END); lseek(fd, 0, SEEK_SET);
  if (out_len) *out_len = (long)sz;
  if (verbose()) debugPrintf("[jni] OpenResource('%s') -> fd=%d len=%ld\n", prefix, fd, (long)sz);
  return fd;
}

/* ---------------- JNIEnv ops ---------------- */
static intptr_t jni_stub(void) { return 0; }
static int jni_GetVersion(void *e) { (void)e; return 0x00010006; }

static void *jni_FindClass(void *e, const char *name) {
  (void)e;
  if (verbose()) debugPrintf("[jni] FindClass(%s)\n", name);
  if (!name) return &cls_generic;
  if (strstr(name, "KarismaBridge")) return &cls_karisma;
  if (strstr(name, "RawResource")) return &cls_rawres;
  if (strstr(name, "FileDescriptor")) return &cls_fd;
  return &cls_generic;
}
static void *jni_GetObjectClass(void *e, void *obj) {
  (void)e; fake_obj *o = obj;
  if (o && o->tag == T_RAWRES) return &cls_rawres;
  if (o && o->tag == T_FD) return &cls_fd;
  return &cls_generic;
}
static void *jni_GetMethodID(void *e, void *cls, const char *n, const char *s) {
  (void)e; (void)cls; (void)s;
  if (verbose()) debugPrintf("[jni] GetMethodID(%s,%s)\n", n, s);
  return (void *)n; /* ID = ponteiro do nome */
}
static void *jni_GetStaticMethodID(void *e, void *cls, const char *n, const char *s) {
  (void)e; (void)cls; (void)s;
  if (verbose()) debugPrintf("[jni] GetStaticMethodID(%s,%s)\n", n, s);
  return (void *)n;
}
static void *jni_GetFieldID(void *e, void *cls, const char *n, const char *s) {
  (void)e; (void)cls; (void)s;
  if (verbose()) debugPrintf("[jni] GetFieldID(%s,%s)\n", n, s);
  return (void *)n;
}
static void *jni_GetStaticFieldID(void *e, void *cls, const char *n, const char *s) {
  (void)e; (void)cls; (void)s;
  if (verbose()) debugPrintf("[jni] GetStaticFieldID(%s,%s)\n", n, s);
  return (void *)n;
}

/* ---- Get*Field (instância): RawResource + FileDescriptor ---- */
static int jni_GetIntField(void *e, void *obj, void *fid) {
  (void)e; fake_obj *o = obj; const char *f = fid;
  if (!o || !f) return 0;
  if (o->tag == T_RAWRES && !strcmp(f, "status")) return o->status;
  if (o->tag == T_FD && !strcmp(f, "descriptor")) return o->fd;
  return 0;
}
static long long jni_GetLongField(void *e, void *obj, void *fid) {
  (void)e; fake_obj *o = obj; const char *f = fid;
  if (o && o->tag == T_RAWRES) {
    if (!strcmp(f, "len")) return o->len;
    if (!strcmp(f, "offset")) return o->off;
  }
  return 0;
}
static void *jni_GetObjectField(void *e, void *obj, void *fid) {
  (void)e; fake_obj *o = obj; const char *f = fid;
  if (o && o->tag == T_RAWRES && !strcmp(f, "jfd")) return o->jfd;
  return NULL;
}
static float jni_GetFloatField(void *e, void *obj, void *fid) { (void)e;(void)obj;(void)fid; return 0.0f; }

/* ---- Get*StaticField: KarismaBridge.mWidth / mHeight ---- */
static int jni_GetStaticIntField(void *e, void *cls, void *fid) {
  (void)e; (void)cls; const char *f = fid;
  if (f && !strcmp(f, "mWidth")) return g_scr_w;
  if (f && !strcmp(f, "mHeight")) return g_scr_h;
  return 0;
}

/* ---- despacho das up-calls static de KarismaBridge ---- */
static int call_static_int(const char *n) {
  if (!n) return 0;
  if (!strcmp(n, "GetSliderState")) return 0;
  if (!strcmp(n, "LowerPerformance")) return 0;   /* não é device fraco */
  if (!strcmp(n, "QualcommDevice")) return 0;
  if (!strcmp(n, "IsXPeriaPlay")) return 0;
  if (!strcmp(n, "GetKeyboardType")) return 0;
  if (!strcmp(n, "GetKeyboardOpened")) return 0;
  if (!strcmp(n, "checkGL20Support")) return 0;   /* força caminho GLES1 */
  if (verbose()) debugPrintf("[jni] CallStaticInt(%s) -> 0 (default)\n", n);
  return 0;
}
static void call_static_void(const char *n, void *arg0) {
  if (!n) return;
  if (!strcmp(n, "EnableSound")) { g_snd_enabled = 1; return; }
  if (!strcmp(n, "DisableSound")) { g_snd_enabled = 0; return; }
  if (!strcmp(n, "LockSound")) { g_snd_locked = 1; return; }
  if (!strcmp(n, "UnlockSound")) { g_snd_locked = 0; return; }
  if (!strcmp(n, "ExitApplication")) { g_want_exit = 1; return; }
  if (!strcmp(n, "OpenURL")) return;
  if (!strcmp(n, "ConfigAssets")) return;     /* dados já pré-colocados */
  if (!strcmp(n, "SetWidthHeightScreen")) return;
  if (!strcmp(n, "onNativeAssertFailed") || !strcmp(n, "onNativeException")) {
    debugPrintf("[jni] *** NATIVE ERROR: %s ***\n", as_cstr(arg0) ? as_cstr(arg0) : "?");
    return;
  }
  if (!strcmp(n, "onNativeDimissDialog")) return;
  if (verbose()) debugPrintf("[jni] CallStaticVoid(%s)\n", n);
}
static void *call_static_obj(const char *n, void *arg0) {
  if (!n) return NULL;
  if (!strcmp(n, "GetAndroidId")) return mk_str("NextOS-BC2-00000000");
  if (!strcmp(n, "OpenResource")) {
    const char *pfx = as_cstr(arg0);
    fake_obj *rr = new_obj(T_RAWRES);
    long len = 0;
    int fd = pfx ? open_asset_prefix(pfx, &len) : -1;
    if (fd >= 0) {
      fake_obj *jfd = new_obj(T_FD); jfd->fd = fd;
      rr->status = 0; rr->len = len; rr->off = 0; rr->jfd = jfd;
    } else {
      rr->status = 1; /* not found */
    }
    return rr;
  }
  if (verbose()) debugPrintf("[jni] CallStaticObj(%s)\n", n);
  return NULL;
}

/* CallStatic* variantes (…, V, A). Lemos no máx 1 arg (OpenResource/erros). */
static void *jni_CallStaticObjectMethod(void *e, void *cls, void *mid, ...) {
  (void)e;(void)cls; va_list ap; va_start(ap, mid); void *a0 = va_arg(ap, void*); va_end(ap);
  return call_static_obj((const char *)mid, a0);
}
static void *jni_CallStaticObjectMethodV(void *e, void *cls, void *mid, va_list ap) {
  (void)e;(void)cls; void *a0 = va_arg(ap, void*); return call_static_obj((const char *)mid, a0);
}
static int jni_CallStaticIntMethod(void *e, void *cls, void *mid, ...) {
  (void)e;(void)cls; return call_static_int((const char *)mid);
}
static int jni_CallStaticIntMethodV(void *e, void *cls, void *mid, va_list ap) {
  (void)e;(void)cls;(void)ap; return call_static_int((const char *)mid);
}
static unsigned char jni_CallStaticBooleanMethod(void *e, void *cls, void *mid, ...) {
  (void)e;(void)cls; return (unsigned char)call_static_int((const char *)mid);
}
static unsigned char jni_CallStaticBooleanMethodV(void *e, void *cls, void *mid, va_list ap) {
  (void)e;(void)cls;(void)ap; return (unsigned char)call_static_int((const char *)mid);
}
static void jni_CallStaticVoidMethod(void *e, void *cls, void *mid, ...) {
  (void)e;(void)cls; va_list ap; va_start(ap, mid); void *a0 = va_arg(ap, void*); va_end(ap);
  call_static_void((const char *)mid, a0);
}
static void jni_CallStaticVoidMethodV(void *e, void *cls, void *mid, va_list ap) {
  (void)e;(void)cls; void *a0 = va_arg(ap, void*); call_static_void((const char *)mid, a0);
}

/* Call* de instância — o engine quase não usa, mas mantemos seguros */
static void *jni_CallObjectMethod(void *e, void *o, void *mid, ...) { (void)e;(void)o;(void)mid; return NULL; }
static int jni_CallIntMethod(void *e, void *o, void *mid, ...) { (void)e;(void)o;(void)mid; return 0; }
static void jni_CallVoidMethod(void *e, void *o, void *mid, ...) { (void)e;(void)o;(void)mid; }
static unsigned char jni_CallBooleanMethod(void *e, void *o, void *mid, ...) { (void)e;(void)o;(void)mid; return 0; }

/* ---- Strings ---- */
static void *jni_NewStringUTF(void *e, const char *s) { (void)e; return mk_str(s); }
static const char *jni_GetStringUTFChars(void *e, void *js, unsigned char *iscopy) {
  (void)e; if (iscopy) *iscopy = 0; return as_cstr(js);
}
static void jni_ReleaseStringUTFChars(void *e, void *js, const char *c) { (void)e;(void)js;(void)c; }
static int jni_GetStringUTFLength(void *e, void *js) { (void)e; const char *s = as_cstr(js); return s ? (int)strlen(s) : 0; }
static int jni_GetStringLength(void *e, void *js) { return jni_GetStringUTFLength(e, js); }

/* ---- Arrays (short[] do updateSound) ---- */
static int jni_GetArrayLength(void *e, void *arr) { (void)e; fake_obj *o = arr; return o && o->tag == T_SHORTARR ? o->alen : 0; }
static void *jni_GetShortArrayElements(void *e, void *arr, unsigned char *iscopy) {
  (void)e; if (iscopy) *iscopy = 0; fake_obj *o = arr; return o && o->tag == T_SHORTARR ? o->data : NULL;
}
static void jni_ReleaseShortArrayElements(void *e, void *arr, void *elems, int mode) { (void)e;(void)arr;(void)elems;(void)mode; }
static void *jni_GetPrimitiveArrayCritical(void *e, void *arr, unsigned char *iscopy) {
  return jni_GetShortArrayElements(e, arr, iscopy);
}
static void jni_ReleasePrimitiveArrayCritical(void *e, void *arr, void *carray, int mode) { (void)e;(void)arr;(void)carray;(void)mode; }
static void jni_GetShortArrayRegion(void *e, void *arr, int start, int len, short *buf) {
  (void)e; fake_obj *o = arr;
  if (o && o->tag == T_SHORTARR && buf) memcpy(buf, o->data + start, (size_t)len * 2);
}
static void jni_SetShortArrayRegion(void *e, void *arr, int start, int len, const short *buf) {
  (void)e; fake_obj *o = arr;
  if (o && o->tag == T_SHORTARR && buf) memcpy(o->data + start, buf, (size_t)len * 2);
}

/* ---- refs / exceptions ---- */
static void *jni_ref(void *e, void *o) { (void)e; return o; }
static void jni_void_ref(void *e, void *o) { (void)e; (void)o; }
static unsigned char jni_IsSameObject(void *e, void *a, void *b) { (void)e; return a == b; }
static void *jni_ExceptionOccurred(void *e) { (void)e; return NULL; }
static void jni_ExceptionClear(void *e) { (void)e; }
static int jni_GetJavaVM(void *e, void **vm) { (void)e; *vm = &vm_ptr; return 0; }

/* ---------------- JavaVM ops ---------------- */
static int vm_GetEnv(void *vm, void **penv, int ver) { (void)vm;(void)ver; *penv = &env_ptr; return 0; }
static int vm_AttachCurrentThread(void *vm, void **penv, void *args) { (void)vm;(void)args; *penv = &env_ptr; return 0; }
static int vm_DetachCurrentThread(void *vm) { (void)vm; return 0; }

/* ---------------- init ---------------- */
void jni_shim_init(void **out_vm, void **out_env) {
  for (int i = 0; i < VT; i++) { env_vt[i] = (uintptr_t)jni_stub; vm_vt[i] = (uintptr_t)jni_stub; }

  env_vt[4]   = (uintptr_t)jni_GetVersion;
  env_vt[6]   = (uintptr_t)jni_FindClass;
  env_vt[13]  = (uintptr_t)jni_stub;                 /* Throw */
  env_vt[14]  = (uintptr_t)jni_stub;                 /* ThrowNew */
  env_vt[15]  = (uintptr_t)jni_ExceptionOccurred;
  env_vt[17]  = (uintptr_t)jni_ExceptionClear;
  env_vt[21]  = (uintptr_t)jni_ref;                  /* NewGlobalRef */
  env_vt[22]  = (uintptr_t)jni_void_ref;             /* DeleteGlobalRef */
  env_vt[23]  = (uintptr_t)jni_void_ref;             /* DeleteLocalRef */
  env_vt[24]  = (uintptr_t)jni_IsSameObject;
  env_vt[25]  = (uintptr_t)jni_ref;                  /* NewLocalRef */
  env_vt[31]  = (uintptr_t)jni_GetObjectClass;
  env_vt[33]  = (uintptr_t)jni_GetMethodID;
  env_vt[34]  = (uintptr_t)jni_CallObjectMethod;
  env_vt[35]  = (uintptr_t)jni_CallObjectMethod;
  env_vt[37]  = (uintptr_t)jni_CallBooleanMethod;
  env_vt[38]  = (uintptr_t)jni_CallBooleanMethod;
  env_vt[49]  = (uintptr_t)jni_CallIntMethod;
  env_vt[50]  = (uintptr_t)jni_CallIntMethod;
  env_vt[61]  = (uintptr_t)jni_CallVoidMethod;
  env_vt[62]  = (uintptr_t)jni_CallVoidMethod;
  env_vt[94]  = (uintptr_t)jni_GetFieldID;
  env_vt[95]  = (uintptr_t)jni_GetObjectField;
  env_vt[100] = (uintptr_t)jni_GetIntField;
  env_vt[101] = (uintptr_t)jni_GetLongField;
  env_vt[102] = (uintptr_t)jni_GetFloatField;
  env_vt[113] = (uintptr_t)jni_GetStaticMethodID;
  env_vt[114] = (uintptr_t)jni_CallStaticObjectMethod;
  env_vt[115] = (uintptr_t)jni_CallStaticObjectMethodV;
  env_vt[117] = (uintptr_t)jni_CallStaticBooleanMethod;
  env_vt[118] = (uintptr_t)jni_CallStaticBooleanMethodV;
  env_vt[129] = (uintptr_t)jni_CallStaticIntMethod;
  env_vt[130] = (uintptr_t)jni_CallStaticIntMethodV;
  env_vt[141] = (uintptr_t)jni_CallStaticVoidMethod;
  env_vt[142] = (uintptr_t)jni_CallStaticVoidMethodV;
  env_vt[144] = (uintptr_t)jni_GetStaticFieldID;
  env_vt[150] = (uintptr_t)jni_GetStaticIntField;
  env_vt[163] = (uintptr_t)jni_NewStringUTF;         /* NewString (reaproveita) */
  env_vt[164] = (uintptr_t)jni_GetStringLength;
  env_vt[167] = (uintptr_t)jni_NewStringUTF;
  env_vt[168] = (uintptr_t)jni_GetStringUTFLength;
  env_vt[169] = (uintptr_t)jni_GetStringUTFChars;
  env_vt[170] = (uintptr_t)jni_ReleaseStringUTFChars;
  env_vt[171] = (uintptr_t)jni_GetArrayLength;
  env_vt[186] = (uintptr_t)jni_GetShortArrayElements;
  env_vt[194] = (uintptr_t)jni_ReleaseShortArrayElements;
  env_vt[202] = (uintptr_t)jni_GetShortArrayRegion;
  env_vt[210] = (uintptr_t)jni_SetShortArrayRegion;
  env_vt[219] = (uintptr_t)jni_GetJavaVM;
  env_vt[222] = (uintptr_t)jni_GetPrimitiveArrayCritical;
  env_vt[223] = (uintptr_t)jni_ReleasePrimitiveArrayCritical;

  vm_vt[3] = (uintptr_t)jni_stub;                    /* DestroyJavaVM */
  vm_vt[4] = (uintptr_t)vm_AttachCurrentThread;
  vm_vt[5] = (uintptr_t)vm_DetachCurrentThread;
  vm_vt[6] = (uintptr_t)vm_GetEnv;
  vm_vt[7] = (uintptr_t)vm_AttachCurrentThread;      /* AsDaemon */

  if (out_vm) *out_vm = &vm_ptr;
  if (out_env) *out_env = &env_ptr;
}
