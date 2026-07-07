// imports.gen.c — GERADO por new-port.sh para 'kof2012a' (libmain.so)
// 106 simbolos. Resolva os UNKNOWN no fim do arquivo.
#include "imports.h"
#include "so_util.h"
#include <stdio.h>

// === passthrough/pthread/shim: ligados automaticamente ===
DynLibFunction dynlib_functions[] = {
  {"SL_IID_ANDROIDSIMPLEBUFFERQUEUE", (uintptr_t)&SL_IID_ANDROIDSIMPLEBUFFERQUEUE_shim},  // opensles_shim
  {"SL_IID_ENGINE", (uintptr_t)&SL_IID_ENGINE_shim},  // opensles_shim
  {"SL_IID_PLAY", (uintptr_t)&SL_IID_PLAY_shim},  // opensles_shim
  {"SL_IID_VOLUME", (uintptr_t)&SL_IID_VOLUME_shim},  // opensles_shim
  {"_ZdaPv", (uintptr_t)&_ZdaPv},  // cxx
  {"_ZdlPv", (uintptr_t)&_ZdlPv},  // cxx
  {"_Znam", (uintptr_t)&_Znam},  // cxx
  {"_Znwm", (uintptr_t)&_Znwm},  // cxx
  {"__android_log_print", (uintptr_t)&__android_log_print},  // liblog
  {"__cxa_atexit", (uintptr_t)&__cxa_atexit},  // cxx
  {"__cxa_finalize", (uintptr_t)&__cxa_finalize},  // cxx
  {"__cxa_guard_acquire", (uintptr_t)&__cxa_guard_acquire},  // cxx
  {"__cxa_guard_release", (uintptr_t)&__cxa_guard_release},  // cxx
  {"__stack_chk_fail", (uintptr_t)&__stack_chk_fail},  // abi
  {"atan2", (uintptr_t)&atan2},  // pass
  {"atan2f", (uintptr_t)&atan2f},  // pass
  {"calloc", (uintptr_t)&calloc},  // pass
  // TODO {"clock", (uintptr_t)&stub_clock},  // <<< IMPLEMENTAR
  {"clock_gettime", (uintptr_t)&clock_gettime},  // pass
  {"cos", (uintptr_t)&cos},  // pass
  {"cosf", (uintptr_t)&cosf},  // pass
  {"exp", (uintptr_t)&exp},  // pass
  // TODO {"exp2f", (uintptr_t)&stub_exp2f},  // <<< IMPLEMENTAR
  {"fclose", (uintptr_t)&fclose},  // pass
  {"fgetc", (uintptr_t)&fgetc},  // pass
  {"fopen", (uintptr_t)&fopen},  // pass
  {"fread", (uintptr_t)&fread},  // pass
  {"free", (uintptr_t)&free},  // pass
  {"fseek", (uintptr_t)&fseek},  // pass
  {"ftell", (uintptr_t)&ftell},  // pass
  {"fwrite", (uintptr_t)&fwrite},  // pass
  {"glAlphaFunc", (uintptr_t)&glAlphaFunc},  // gles
  {"glBindTexture", (uintptr_t)&glBindTexture},  // gles
  {"glBlendFunc", (uintptr_t)&glBlendFunc},  // gles
  {"glClear", (uintptr_t)&glClear},  // gles
  {"glClearColor", (uintptr_t)&glClearColor},  // gles
  {"glColor4f", (uintptr_t)&glColor4f},  // gles
  {"glCompressedTexImage2D", (uintptr_t)&glCompressedTexImage2D},  // gles
  {"glCopyTexImage2D", (uintptr_t)&glCopyTexImage2D},  // gles
  {"glDeleteTextures", (uintptr_t)&glDeleteTextures},  // gles
  {"glDepthFunc", (uintptr_t)&glDepthFunc},  // gles
  {"glDepthMask", (uintptr_t)&glDepthMask},  // gles
  {"glDisable", (uintptr_t)&glDisable},  // gles
  {"glDisableClientState", (uintptr_t)&glDisableClientState},  // gles
  {"glDrawArrays", (uintptr_t)&glDrawArrays},  // gles
  {"glDrawElements", (uintptr_t)&glDrawElements},  // gles
  {"glEnable", (uintptr_t)&glEnable},  // gles
  {"glEnableClientState", (uintptr_t)&glEnableClientState},  // gles
  {"glFlush", (uintptr_t)&glFlush},  // gles
  {"glFogf", (uintptr_t)&glFogf},  // gles
  {"glFogfv", (uintptr_t)&glFogfv},  // gles
  {"glGenTextures", (uintptr_t)&glGenTextures},  // gles
  {"glGetError", (uintptr_t)&glGetError},  // gles
  {"glGetString", (uintptr_t)&glGetString},  // gles
  {"glHint", (uintptr_t)&glHint},  // gles
  {"glLoadIdentity", (uintptr_t)&glLoadIdentity},  // gles
  {"glMatrixMode", (uintptr_t)&glMatrixMode},  // gles
  {"glOrthof", (uintptr_t)&glOrthof},  // gles
  {"glPixelStorei", (uintptr_t)&glPixelStorei},  // gles
  {"glReadPixels", (uintptr_t)&glReadPixels},  // gles
  {"glScissor", (uintptr_t)&glScissor},  // gles
  {"glShadeModel", (uintptr_t)&glShadeModel},  // gles
  {"glTexCoordPointer", (uintptr_t)&glTexCoordPointer},  // gles
  {"glTexImage2D", (uintptr_t)&glTexImage2D},  // gles
  {"glTexParameterx", (uintptr_t)&glTexParameterx},  // gles
  {"glTexSubImage2D", (uintptr_t)&glTexSubImage2D},  // gles
  {"glVertexPointer", (uintptr_t)&glVertexPointer},  // gles
  {"glViewport", (uintptr_t)&glViewport},  // gles
  {"ldexp", (uintptr_t)&ldexp},  // pass
  {"localtime", (uintptr_t)&localtime},  // pass
  {"log", (uintptr_t)&log},  // pass
  {"malloc", (uintptr_t)&malloc},  // pass
  {"memcmp", (uintptr_t)&memcmp},  // pass
  {"memcpy", (uintptr_t)&memcpy},  // pass
  {"memmove", (uintptr_t)&memmove},  // pass
  {"memset", (uintptr_t)&memset},  // pass
  {"mktime", (uintptr_t)&mktime},  // pass
  {"pow", (uintptr_t)&pow},  // pass
  {"printf", (uintptr_t)&printf},  // pass
  {"pthread_create", (uintptr_t)&pthread_create_fake},  // pthread wrapper (core)
  {"pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy_fake},  // pthread wrapper (core)
  {"pthread_mutex_init", (uintptr_t)&pthread_mutex_init_fake},  // pthread wrapper (core)
  {"pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock_fake},  // pthread wrapper (core)
  {"pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_fake},  // pthread wrapper (core)
  {"qsort", (uintptr_t)&qsort},  // pass
  {"realloc", (uintptr_t)&realloc},  // pass
  // TODO {"select", (uintptr_t)&stub_select},  // <<< IMPLEMENTAR
  {"sin", (uintptr_t)&sin},  // pass
  {"slCreateEngine", (uintptr_t)&slCreateEngine_shim},  // opensles_shim
  {"sprintf", (uintptr_t)&sprintf},  // pass
  {"sqrt", (uintptr_t)&sqrt},  // pass
  {"sqrtf", (uintptr_t)&sqrtf},  // pass
  {"strcmp", (uintptr_t)&strcmp},  // pass
  {"strcpy", (uintptr_t)&strcpy},  // pass
  {"strlen", (uintptr_t)&strlen},  // pass
  {"strncpy", (uintptr_t)&strncpy},  // pass
  {"strstr", (uintptr_t)&strstr},  // pass
  {"time", (uintptr_t)&time},  // pass
  {"usleep", (uintptr_t)&usleep},  // pass
};
const int dynlib_functions_count = sizeof(dynlib_functions)/sizeof(dynlib_functions[0]);

// ===================== SIMBOLOS A IMPLEMENTAR =====================
//   clock
//   exp2f
//   select
