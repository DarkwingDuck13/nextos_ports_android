// imports_unity.gen.c — GERADO. passthrough via dlsym; resto = stub log.
#include "so_util.h"
#include <stdio.h>
#include <stdint.h>
#include <dlfcn.h>

// system properties FALSAS (bionic_shims.c) — repontadas dos stubs vazios
extern int __system_property_get(const char *, char *);
extern const void *__system_property_find(const char *);
extern int __system_property_read(const void *, char *, char *);
extern void __system_property_read_callback(const void *, void (*)(void*, const char*, const char*, unsigned), void *);
extern int *__errno(void);

// stubs (logam nome, 1as 2 vezes, retornam 0)
long stub_ALooper_acquire(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ALooper_acquire\\n"); return 0; }
long stub_ALooper_forThread(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ALooper_forThread\\n"); return 0; }
long stub_ALooper_pollAll(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ALooper_pollAll\\n"); return 0; }
long stub_ALooper_prepare(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ALooper_prepare\\n"); return 0; }
long stub_ALooper_release(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ALooper_release\\n"); return 0; }
long stub_ALooper_wake(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ALooper_wake\\n"); return 0; }
long stub_ANativeWindow_acquire(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ANativeWindow_acquire\\n"); return 0; }
long stub_ANativeWindow_fromSurface(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ANativeWindow_fromSurface\\n"); return 0; }
long stub_ANativeWindow_getHeight(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ANativeWindow_getHeight\\n"); return 0; }
long stub_ANativeWindow_getWidth(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ANativeWindow_getWidth\\n"); return 0; }
long stub_ANativeWindow_release(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ANativeWindow_release\\n"); return 0; }
long stub_ANativeWindow_setBuffersGeometry(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ANativeWindow_setBuffersGeometry\\n"); return 0; }
long stub___android_log_print(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __android_log_print\\n"); return 0; }
long stub___android_log_vprint(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __android_log_vprint\\n"); return 0; }
long stub___android_log_write(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __android_log_write\\n"); return 0; }
long stub_ASensorEventQueue_disableSensor(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ASensorEventQueue_disableSensor\\n"); return 0; }
long stub_ASensorEventQueue_enableSensor(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ASensorEventQueue_enableSensor\\n"); return 0; }
long stub_ASensorEventQueue_getEvents(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ASensorEventQueue_getEvents\\n"); return 0; }
long stub_ASensorEventQueue_hasEvents(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ASensorEventQueue_hasEvents\\n"); return 0; }
long stub_ASensorEventQueue_setEventRate(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ASensorEventQueue_setEventRate\\n"); return 0; }
long stub_ASensor_getMinDelay(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ASensor_getMinDelay\\n"); return 0; }
long stub_ASensor_getName(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ASensor_getName\\n"); return 0; }
long stub_ASensor_getResolution(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ASensor_getResolution\\n"); return 0; }
long stub_ASensor_getType(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ASensor_getType\\n"); return 0; }
long stub_ASensor_getVendor(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ASensor_getVendor\\n"); return 0; }
long stub_ASensorManager_createEventQueue(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ASensorManager_createEventQueue\\n"); return 0; }
long stub_ASensorManager_destroyEventQueue(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ASensorManager_destroyEventQueue\\n"); return 0; }
long stub_ASensorManager_getDefaultSensor(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ASensorManager_getDefaultSensor\\n"); return 0; }
long stub_ASensorManager_getInstance(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ASensorManager_getInstance\\n"); return 0; }
long stub_ASensorManager_getSensorList(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ASensorManager_getSensorList\\n"); return 0; }
long stub_connect(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] connect\\n"); return 0; }
long stub___ctype_get_mb_cur_max(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __ctype_get_mb_cur_max\\n"); return 0; }
long stub___cxa_atexit(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __cxa_atexit\\n"); return 0; }
long stub___cxa_finalize(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __cxa_finalize\\n"); return 0; }
long stub_dladdr(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] dladdr\\n"); return 0; }
long stub_dl_iterate_phdr(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] dl_iterate_phdr\\n"); return 0; }
long stub_environ(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] environ\\n"); return 0; }
long stub___errno(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __errno\\n"); return 0; }
long stub_ExecuteProgram(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ExecuteProgram\\n"); return 0; }
long stub_exp2f(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] exp2f\\n"); return 0; }
long stub___FD_ISSET_chk(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __FD_ISSET_chk\\n"); return 0; }
long stub___FD_SET_chk(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __FD_SET_chk\\n"); return 0; }
long stub_getegid(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] getegid\\n"); return 0; }
long stub_inflate(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] inflate\\n"); return 0; }
long stub_inflateEnd(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] inflateEnd\\n"); return 0; }
long stub_inflateInit2_(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] inflateInit2_\\n"); return 0; }
long stub_ldexpf(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ldexpf\\n"); return 0; }
long stub_lldiv(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] lldiv\\n"); return 0; }
long stub_logb(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] logb\\n"); return 0; }
long stub___memmove_chk(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __memmove_chk\\n"); return 0; }
long stub_modff(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] modff\\n"); return 0; }
long stub_recv(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] recv\\n"); return 0; }
long stub_scalbn(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] scalbn\\n"); return 0; }
long stub___sF(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __sF\\n"); return 0; }
long stub_sigaction(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] sigaction\\n"); return 0; }
long stub_signal(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] signal\\n"); return 0; }
long stub_sigsuspend(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] sigsuspend\\n"); return 0; }
long stub_sincos(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] sincos\\n"); return 0; }
long stub_sincosf(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] sincosf\\n"); return 0; }
long stub_socket(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] socket\\n"); return 0; }
long stub_srand48(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] srand48\\n"); return 0; }
long stub___stack_chk_fail(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __stack_chk_fail\\n"); return 0; }
long stub_strcasestr(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] strcasestr\\n"); return 0; }
long stub___strlen_chk(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __strlen_chk\\n"); return 0; }
long stub_strtoull_l(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] strtoull_l\\n"); return 0; }
long stub___system_property_find(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __system_property_find\\n"); return 0; }
long stub___system_property_get(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __system_property_get\\n"); return 0; }
long stub___system_property_read(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __system_property_read\\n"); return 0; }
long stub_utimes(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] utimes\\n"); return 0; }
long stub___vsnprintf_chk(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __vsnprintf_chk\\n"); return 0; }

// flag: 1 = passthrough (resolver via dlsym), 0 = stub ja setado
static const char *passthrough_names[] = {
  "acos",
  "acosf",
  "asin",
  "asinf",
  "atan",
  "atan2",
  "atan2f",
  "atanf",
  "calloc",
  "close",
  "cos",
  "cosf",
  "dlclose",
  "dlerror",
  "dlopen",
  "dlsym",
  "eglChooseConfig",
  "eglCreateContext",
  "eglCreatePbufferSurface",
  "eglCreateWindowSurface",
  "eglDestroyContext",
  "eglDestroySurface",
  "eglGetConfigAttrib",
  "eglGetCurrentContext",
  "eglGetCurrentSurface",
  "eglGetDisplay",
  "eglGetError",
  "eglGetProcAddress",
  "eglInitialize",
  "eglMakeCurrent",
  "eglQueryString",
  "eglQuerySurface",
  "eglSurfaceAttrib",
  "eglSwapBuffers",
  "eglSwapInterval",
  "eglTerminate",
  "_exit",
  "exp",
  "expf",
  "fileno",
  "fmod",
  "fmodf",
  "fopen",
  "fputc",
  "fread",
  "free",
  "fseek",
  "fwrite",
  "getenv",
  "gettid",
  "getuid",
  "hypot",
  "isalnum",
  "iswlower",
  "log",
  "log10",
  "log10f",
  "log2",
  "log2f",
  "logf",
  "longjmp",
  "malloc",
  "mbrlen",
  "memchr",
  "memcmp",
  "memset",
  "modf",
  "open",
  "perror",
  "pow",
  "powf",
  "pthread_condattr_init",
  "pthread_create",
  "pthread_equal",
  "pthread_getattr_np",
  "pthread_key_create",
  "pthread_once",
  "pthread_setname_np",
  "qsort",
  "realloc",
  "setjmp",
  "sigaddset",
  "sigaltstack",
  "sigdelset",
  "sigemptyset",
  "sigfillset",
  "sin",
  "sinf",
  "sqrtf",
  "strcasecmp",
  "strcat",
  "strchr",
  "strcmp",
  "strcpy",
  "strdup",
  "strlen",
  "strncmp",
  "strrchr",
  "strtoul",
  "tan",
  "tanf",
  "uselocale",
  "wmemcpy",
  0 };

DynLibFunction dynlib_functions[] = {
  {"acos", 0},
  {"acosf", 0},
  {"ALooper_acquire", (uintptr_t)&stub_ALooper_acquire},
  {"ALooper_forThread", (uintptr_t)&stub_ALooper_forThread},
  {"ALooper_pollAll", (uintptr_t)&stub_ALooper_pollAll},
  {"ALooper_prepare", (uintptr_t)&stub_ALooper_prepare},
  {"ALooper_release", (uintptr_t)&stub_ALooper_release},
  {"ALooper_wake", (uintptr_t)&stub_ALooper_wake},
  {"ANativeWindow_acquire", (uintptr_t)&stub_ANativeWindow_acquire},
  {"ANativeWindow_fromSurface", (uintptr_t)&stub_ANativeWindow_fromSurface},
  {"ANativeWindow_getHeight", (uintptr_t)&stub_ANativeWindow_getHeight},
  {"ANativeWindow_getWidth", (uintptr_t)&stub_ANativeWindow_getWidth},
  {"ANativeWindow_release", (uintptr_t)&stub_ANativeWindow_release},
  {"ANativeWindow_setBuffersGeometry", (uintptr_t)&stub_ANativeWindow_setBuffersGeometry},
  {"__android_log_print", (uintptr_t)&stub___android_log_print},
  {"__android_log_vprint", (uintptr_t)&stub___android_log_vprint},
  {"__android_log_write", (uintptr_t)&stub___android_log_write},
  {"ASensorEventQueue_disableSensor", (uintptr_t)&stub_ASensorEventQueue_disableSensor},
  {"ASensorEventQueue_enableSensor", (uintptr_t)&stub_ASensorEventQueue_enableSensor},
  {"ASensorEventQueue_getEvents", (uintptr_t)&stub_ASensorEventQueue_getEvents},
  {"ASensorEventQueue_hasEvents", (uintptr_t)&stub_ASensorEventQueue_hasEvents},
  {"ASensorEventQueue_setEventRate", (uintptr_t)&stub_ASensorEventQueue_setEventRate},
  {"ASensor_getMinDelay", (uintptr_t)&stub_ASensor_getMinDelay},
  {"ASensor_getName", (uintptr_t)&stub_ASensor_getName},
  {"ASensor_getResolution", (uintptr_t)&stub_ASensor_getResolution},
  {"ASensor_getType", (uintptr_t)&stub_ASensor_getType},
  {"ASensor_getVendor", (uintptr_t)&stub_ASensor_getVendor},
  {"ASensorManager_createEventQueue", (uintptr_t)&stub_ASensorManager_createEventQueue},
  {"ASensorManager_destroyEventQueue", (uintptr_t)&stub_ASensorManager_destroyEventQueue},
  {"ASensorManager_getDefaultSensor", (uintptr_t)&stub_ASensorManager_getDefaultSensor},
  {"ASensorManager_getInstance", (uintptr_t)&stub_ASensorManager_getInstance},
  {"ASensorManager_getSensorList", (uintptr_t)&stub_ASensorManager_getSensorList},
  {"asin", 0},
  {"asinf", 0},
  {"atan", 0},
  {"atan2", 0},
  {"atan2f", 0},
  {"atanf", 0},
  {"calloc", 0},
  {"close", 0},
  {"connect", (uintptr_t)&stub_connect},
  {"cos", 0},
  {"cosf", 0},
  {"__ctype_get_mb_cur_max", (uintptr_t)&stub___ctype_get_mb_cur_max},
  {"__cxa_atexit", (uintptr_t)&stub___cxa_atexit},
  {"__cxa_finalize", (uintptr_t)&stub___cxa_finalize},
  {"dladdr", (uintptr_t)&stub_dladdr},
  {"dlclose", 0},
  {"dlerror", 0},
  {"dl_iterate_phdr", (uintptr_t)&stub_dl_iterate_phdr},
  {"dlopen", 0},
  {"dlsym", 0},
  {"eglChooseConfig", 0},
  {"eglCreateContext", 0},
  {"eglCreatePbufferSurface", 0},
  {"eglCreateWindowSurface", 0},
  {"eglDestroyContext", 0},
  {"eglDestroySurface", 0},
  {"eglGetConfigAttrib", 0},
  {"eglGetCurrentContext", 0},
  {"eglGetCurrentSurface", 0},
  {"eglGetDisplay", 0},
  {"eglGetError", 0},
  {"eglGetProcAddress", 0},
  {"eglInitialize", 0},
  {"eglMakeCurrent", 0},
  {"eglQueryString", 0},
  {"eglQuerySurface", 0},
  {"eglSurfaceAttrib", 0},
  {"eglSwapBuffers", 0},
  {"eglSwapInterval", 0},
  {"eglTerminate", 0},
  {"environ", (uintptr_t)&stub_environ},
  {"__errno", (uintptr_t)&__errno},
  {"ExecuteProgram", (uintptr_t)&stub_ExecuteProgram},
  {"_exit", 0},
  {"exp", 0},
  {"exp2f", (uintptr_t)&stub_exp2f},
  {"expf", 0},
  {"__FD_ISSET_chk", (uintptr_t)&stub___FD_ISSET_chk},
  {"__FD_SET_chk", (uintptr_t)&stub___FD_SET_chk},
  {"fileno", 0},
  {"fmod", 0},
  {"fmodf", 0},
  {"fopen", 0},
  {"fputc", 0},
  {"fread", 0},
  {"free", 0},
  {"fseek", 0},
  {"fwrite", 0},
  {"getegid", (uintptr_t)&stub_getegid},
  {"getenv", 0},
  {"gettid", 0},
  {"getuid", 0},
  {"hypot", 0},
  {"inflate", (uintptr_t)&stub_inflate},
  {"inflateEnd", (uintptr_t)&stub_inflateEnd},
  {"inflateInit2_", (uintptr_t)&stub_inflateInit2_},
  {"isalnum", 0},
  {"iswlower", 0},
  {"ldexpf", (uintptr_t)&stub_ldexpf},
  {"lldiv", (uintptr_t)&stub_lldiv},
  {"log", 0},
  {"log10", 0},
  {"log10f", 0},
  {"log2", 0},
  {"log2f", 0},
  {"logb", (uintptr_t)&stub_logb},
  {"logf", 0},
  {"longjmp", 0},
  {"malloc", 0},
  {"mbrlen", 0},
  {"memchr", 0},
  {"memcmp", 0},
  {"__memmove_chk", (uintptr_t)&stub___memmove_chk},
  {"memset", 0},
  {"modf", 0},
  {"modff", (uintptr_t)&stub_modff},
  {"open", 0},
  {"perror", 0},
  {"pow", 0},
  {"powf", 0},
  {"pthread_condattr_init", 0},
  {"pthread_create", 0},
  {"pthread_equal", 0},
  {"pthread_getattr_np", 0},
  {"pthread_key_create", 0},
  {"pthread_once", 0},
  {"pthread_setname_np", 0},
  {"qsort", 0},
  {"realloc", 0},
  {"recv", (uintptr_t)&stub_recv},
  {"scalbn", (uintptr_t)&stub_scalbn},
  {"setjmp", 0},
  {"__sF", (uintptr_t)&stub___sF},
  {"sigaction", (uintptr_t)&stub_sigaction},
  {"sigaddset", 0},
  {"sigaltstack", 0},
  {"sigdelset", 0},
  {"sigemptyset", 0},
  {"sigfillset", 0},
  {"signal", (uintptr_t)&stub_signal},
  {"sigsuspend", (uintptr_t)&stub_sigsuspend},
  {"sin", 0},
  {"sincos", (uintptr_t)&stub_sincos},
  {"sincosf", (uintptr_t)&stub_sincosf},
  {"sinf", 0},
  {"socket", (uintptr_t)&stub_socket},
  {"sqrtf", 0},
  {"srand48", (uintptr_t)&stub_srand48},
  {"__stack_chk_fail", (uintptr_t)&stub___stack_chk_fail},
  {"strcasecmp", 0},
  {"strcasestr", (uintptr_t)&stub_strcasestr},
  {"strcat", 0},
  {"strchr", 0},
  {"strcmp", 0},
  {"strcpy", 0},
  {"strdup", 0},
  {"strlen", 0},
  {"__strlen_chk", (uintptr_t)&stub___strlen_chk},
  {"strncmp", 0},
  {"strrchr", 0},
  {"strtoul", 0},
  {"strtoull_l", (uintptr_t)&stub_strtoull_l},
  {"__system_property_find", (uintptr_t)&__system_property_find},
  {"__system_property_get", (uintptr_t)&__system_property_get},
  {"__system_property_read", (uintptr_t)&__system_property_read},
  {"__system_property_read_callback", (uintptr_t)&__system_property_read_callback},
  {"tan", 0},
  {"tanf", 0},
  {"uselocale", 0},
  {"utimes", (uintptr_t)&stub_utimes},
  {"__vsnprintf_chk", (uintptr_t)&stub___vsnprintf_chk},
  {"wmemcpy", 0},
};
size_t dynlib_numfunctions = sizeof(dynlib_functions)/sizeof(dynlib_functions[0]);

// resolve os passthrough via dlsym(RTLD_DEFAULT) em runtime
void recon_fill_passthrough(void){
  for(size_t i=0;i<dynlib_numfunctions;i++){
    if(dynlib_functions[i].func==0){
      void *p = dlsym(RTLD_DEFAULT, dynlib_functions[i].symbol);
      if(p) dynlib_functions[i].func=(uintptr_t)p;
      else fprintf(stderr,"[passthrough FALHOU dlsym] %s\\n", dynlib_functions[i].symbol);
    }
  }
}
