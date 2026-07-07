// imports.gen.c — GERADO por new-port.sh para 'legobatman2' (libLEGO_SH1.so)
// 220 simbolos. Resolva os UNKNOWN no fim do arquivo.
#include "imports.h"
#include "so_util.h"
#include <stdio.h>

// === passthrough/pthread/shim: ligados automaticamente ===
DynLibFunction dynlib_functions[] = {
  {"abort", (uintptr_t)&abort},  // pass
  // TODO {"acosf", (uintptr_t)&stub_acosf},  // <<< IMPLEMENTAR
  {"__android_log_print", (uintptr_t)&__android_log_print},  // liblog
  // TODO {"android_set_abort_message", (uintptr_t)&stub_android_set_abort_message},  // <<< IMPLEMENTAR
  // TODO {"asinf", (uintptr_t)&stub_asinf},  // <<< IMPLEMENTAR
  {"atan2f", (uintptr_t)&atan2f},  // pass
  // TODO {"atanf", (uintptr_t)&stub_atanf},  // <<< IMPLEMENTAR
  {"atof", (uintptr_t)&atof},  // pass
  {"atoi", (uintptr_t)&atoi},  // pass
  {"bsearch", (uintptr_t)&bsearch},  // pass
  {"calloc", (uintptr_t)&calloc},  // pass
  // TODO {"ceill", (uintptr_t)&stub_ceill},  // <<< IMPLEMENTAR
  {"close", (uintptr_t)&close},  // pass
  // TODO {"closelog", (uintptr_t)&stub_closelog},  // <<< IMPLEMENTAR
  {"cosf", (uintptr_t)&cosf},  // pass
  // TODO {"_ctype_", (uintptr_t)&stub__ctype_},  // <<< IMPLEMENTAR
  {"__cxa_atexit", (uintptr_t)&__cxa_atexit},  // cxx
  {"__cxa_finalize", (uintptr_t)&__cxa_finalize},  // cxx
  // TODO {"dl_iterate_phdr", (uintptr_t)&stub_dl_iterate_phdr},  // <<< IMPLEMENTAR
  {"eglBindAPI", (uintptr_t)&eglBindAPI_shim},  // egl_shim
  {"eglChooseConfig", (uintptr_t)&eglChooseConfig_shim},  // egl_shim
  {"eglCreateContext", (uintptr_t)&eglCreateContext_shim},  // egl_shim
  {"eglCreatePbufferSurface", (uintptr_t)&eglCreatePbufferSurface_shim},  // egl_shim
  {"eglGetCurrentContext", (uintptr_t)&eglGetCurrentContext_shim},  // egl_shim
  {"eglGetCurrentDisplay", (uintptr_t)&eglGetCurrentDisplay_shim},  // egl_shim
  {"eglGetCurrentSurface", (uintptr_t)&eglGetCurrentSurface_shim},  // egl_shim
  {"eglGetError", (uintptr_t)&eglGetError_shim},  // egl_shim
  {"eglGetProcAddress", (uintptr_t)&eglGetProcAddress_shim},  // egl_shim
  {"eglMakeCurrent", (uintptr_t)&eglMakeCurrent_shim},  // egl_shim
  {"eglSwapInterval", (uintptr_t)&eglSwapInterval_shim},  // egl_shim
  {"__errno", (uintptr_t)&__errno},  // pass
  {"fclose", (uintptr_t)&fclose},  // pass
  // TODO {"ferror", (uintptr_t)&stub_ferror},  // <<< IMPLEMENTAR
  {"fflush", (uintptr_t)&fflush},  // pass
  {"fgetc", (uintptr_t)&fgetc},  // pass
  {"fileno", (uintptr_t)&fileno},  // pass
  // TODO {"floorl", (uintptr_t)&stub_floorl},  // <<< IMPLEMENTAR
  // TODO {"fmodl", (uintptr_t)&stub_fmodl},  // <<< IMPLEMENTAR
  {"fopen", (uintptr_t)&fopen},  // pass
  {"fprintf", (uintptr_t)&fprintf},  // pass
  {"fputc", (uintptr_t)&fputc},  // pass
  {"fread", (uintptr_t)&fread},  // pass
  {"free", (uintptr_t)&free},  // pass
  {"fseek", (uintptr_t)&fseek},  // pass
  {"ftell", (uintptr_t)&ftell},  // pass
  {"fwrite", (uintptr_t)&fwrite},  // pass
  // TODO {"getauxval", (uintptr_t)&stub_getauxval},  // <<< IMPLEMENTAR
  {"gettimeofday", (uintptr_t)&gettimeofday},  // pass
  {"glActiveTexture", (uintptr_t)&glActiveTexture},  // gles
  {"glAttachShader", (uintptr_t)&glAttachShader},  // gles
  {"glBindBuffer", (uintptr_t)&glBindBuffer},  // gles
  {"glBindFramebuffer", (uintptr_t)&glBindFramebuffer},  // gles
  {"glBindRenderbuffer", (uintptr_t)&glBindRenderbuffer},  // gles
  {"glBindTexture", (uintptr_t)&glBindTexture},  // gles
  {"glBlendFunc", (uintptr_t)&glBlendFunc},  // gles
  {"glBufferData", (uintptr_t)&glBufferData},  // gles
  {"glClear", (uintptr_t)&glClear},  // gles
  {"glClearColor", (uintptr_t)&glClearColor},  // gles
  {"glClearDepthf", (uintptr_t)&glClearDepthf},  // gles
  {"glClearStencil", (uintptr_t)&glClearStencil},  // gles
  {"glCompileShader", (uintptr_t)&glCompileShader},  // gles
  {"glCompressedTexImage2D", (uintptr_t)&glCompressedTexImage2D},  // gles
  {"glCreateProgram", (uintptr_t)&glCreateProgram},  // gles
  {"glCreateShader", (uintptr_t)&glCreateShader},  // gles
  {"glDeleteBuffers", (uintptr_t)&glDeleteBuffers},  // gles
  {"glDeleteFramebuffers", (uintptr_t)&glDeleteFramebuffers},  // gles
  {"glDeleteProgram", (uintptr_t)&glDeleteProgram},  // gles
  {"glDeleteRenderbuffers", (uintptr_t)&glDeleteRenderbuffers},  // gles
  {"glDeleteShader", (uintptr_t)&glDeleteShader},  // gles
  {"glDeleteTextures", (uintptr_t)&glDeleteTextures},  // gles
  {"glDepthFunc", (uintptr_t)&glDepthFunc},  // gles
  {"glDepthMask", (uintptr_t)&glDepthMask},  // gles
  {"glDepthRangef", (uintptr_t)&glDepthRangef},  // gles
  {"glDisable", (uintptr_t)&glDisable},  // gles
  {"glDrawArrays", (uintptr_t)&glDrawArrays},  // gles
  {"glDrawElements", (uintptr_t)&glDrawElements},  // gles
  {"glEnable", (uintptr_t)&glEnable},  // gles
  {"glEnableVertexAttribArray", (uintptr_t)&glEnableVertexAttribArray},  // gles
  {"glFinish", (uintptr_t)&glFinish},  // gles
  {"glFlush", (uintptr_t)&glFlush},  // gles
  {"glFramebufferRenderbuffer", (uintptr_t)&glFramebufferRenderbuffer},  // gles
  {"glFramebufferTexture2D", (uintptr_t)&glFramebufferTexture2D},  // gles
  {"glFrontFace", (uintptr_t)&glFrontFace},  // gles
  {"glGenBuffers", (uintptr_t)&glGenBuffers},  // gles
  {"glGenFramebuffers", (uintptr_t)&glGenFramebuffers},  // gles
  {"glGenRenderbuffers", (uintptr_t)&glGenRenderbuffers},  // gles
  {"glGenTextures", (uintptr_t)&glGenTextures},  // gles
  {"glGetActiveAttrib", (uintptr_t)&glGetActiveAttrib},  // gles
  {"glGetActiveUniform", (uintptr_t)&glGetActiveUniform},  // gles
  {"glGetAttribLocation", (uintptr_t)&glGetAttribLocation},  // gles
  {"glGetBufferParameteriv", (uintptr_t)&glGetBufferParameteriv},  // gles
  {"glGetError", (uintptr_t)&glGetError},  // gles
  {"glGetIntegerv", (uintptr_t)&glGetIntegerv},  // gles
  {"glGetProgramInfoLog", (uintptr_t)&glGetProgramInfoLog},  // gles
  {"glGetProgramiv", (uintptr_t)&glGetProgramiv},  // gles
  {"glGetShaderInfoLog", (uintptr_t)&glGetShaderInfoLog},  // gles
  {"glGetShaderiv", (uintptr_t)&glGetShaderiv},  // gles
  {"glGetString", (uintptr_t)&glGetString},  // gles
  {"glGetUniformLocation", (uintptr_t)&glGetUniformLocation},  // gles
  {"glLinkProgram", (uintptr_t)&glLinkProgram},  // gles
  {"glRenderbufferStorage", (uintptr_t)&glRenderbufferStorage},  // gles
  {"glScissor", (uintptr_t)&glScissor},  // gles
  {"glShaderSource", (uintptr_t)&glShaderSource},  // gles
  {"glStencilMask", (uintptr_t)&glStencilMask},  // gles
  {"glTexImage2D", (uintptr_t)&glTexImage2D},  // gles
  {"glTexParameteri", (uintptr_t)&glTexParameteri},  // gles
  {"glUniform1i", (uintptr_t)&glUniform1i},  // gles
  {"glUniform3fv", (uintptr_t)&glUniform3fv},  // gles
  {"glUniform4fv", (uintptr_t)&glUniform4fv},  // gles
  {"glUniformMatrix4fv", (uintptr_t)&glUniformMatrix4fv},  // gles
  {"glUseProgram", (uintptr_t)&glUseProgram},  // gles
  {"glVertexAttribPointer", (uintptr_t)&glVertexAttribPointer},  // gles
  {"glViewport", (uintptr_t)&glViewport},  // gles
  {"localtime", (uintptr_t)&localtime},  // pass
  // TODO {"log10f", (uintptr_t)&stub_log10f},  // <<< IMPLEMENTAR
  // TODO {"log10l", (uintptr_t)&stub_log10l},  // <<< IMPLEMENTAR
  {"malloc", (uintptr_t)&malloc},  // pass
  {"memchr", (uintptr_t)&memchr},  // pass
  {"memcmp", (uintptr_t)&memcmp},  // pass
  {"memcpy", (uintptr_t)&memcpy},  // pass
  // TODO {"__memcpy_chk", (uintptr_t)&stub___memcpy_chk},  // <<< IMPLEMENTAR
  {"memmove", (uintptr_t)&memmove},  // pass
  // TODO {"__memmove_chk", (uintptr_t)&stub___memmove_chk},  // <<< IMPLEMENTAR
  {"memset", (uintptr_t)&memset},  // pass
  {"mkdir", (uintptr_t)&mkdir},  // pass
  // TODO {"__open_2", (uintptr_t)&stub___open_2},  // <<< IMPLEMENTAR
  // TODO {"openlog", (uintptr_t)&stub_openlog},  // <<< IMPLEMENTAR
  {"posix_memalign", (uintptr_t)&posix_memalign},  // pass
  {"powf", (uintptr_t)&powf},  // pass
  // TODO {"powl", (uintptr_t)&stub_powl},  // <<< IMPLEMENTAR
  {"pthread_attr_destroy", (uintptr_t)&pthread_attr_destroy_fake},  // pthread wrapper (core)
  {"pthread_attr_init", (uintptr_t)&pthread_attr_init_fake},  // pthread wrapper (core)
  {"pthread_attr_setdetachstate", (uintptr_t)&pthread_attr_setdetachstate_fake},  // pthread wrapper (core)
  {"pthread_cond_broadcast", (uintptr_t)&pthread_cond_broadcast_fake},  // pthread wrapper (core)
  {"pthread_cond_destroy", (uintptr_t)&pthread_cond_destroy_fake},  // pthread wrapper (core)
  {"pthread_cond_init", (uintptr_t)&pthread_cond_init_fake},  // pthread wrapper (core)
  {"pthread_cond_signal", (uintptr_t)&pthread_cond_signal_fake},  // pthread wrapper (core)
  {"pthread_cond_wait", (uintptr_t)&pthread_cond_wait_fake},  // pthread wrapper (core)
  {"pthread_create", (uintptr_t)&pthread_create_fake},  // pthread wrapper (core)
  {"pthread_equal", (uintptr_t)&pthread_equal_fake},  // pthread wrapper (core)
  {"pthread_getschedparam", (uintptr_t)&pthread_getschedparam_fake},  // pthread wrapper (core)
  {"pthread_getspecific", (uintptr_t)&pthread_getspecific_fake},  // pthread wrapper (core)
  {"pthread_join", (uintptr_t)&pthread_join_fake},  // pthread wrapper (core)
  {"pthread_key_create", (uintptr_t)&pthread_key_create_fake},  // pthread wrapper (core)
  {"pthread_key_delete", (uintptr_t)&pthread_key_delete_fake},  // pthread wrapper (core)
  {"pthread_mutexattr_init", (uintptr_t)&pthread_mutexattr_init_fake},  // pthread wrapper (core)
  {"pthread_mutexattr_settype", (uintptr_t)&pthread_mutexattr_settype_fake},  // pthread wrapper (core)
  {"pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy_fake},  // pthread wrapper (core)
  {"pthread_mutex_init", (uintptr_t)&pthread_mutex_init_fake},  // pthread wrapper (core)
  {"pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock_fake},  // pthread wrapper (core)
  {"pthread_mutex_trylock", (uintptr_t)&pthread_mutex_trylock_fake},  // pthread wrapper (core)
  {"pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_fake},  // pthread wrapper (core)
  {"pthread_once", (uintptr_t)&pthread_once_fake},  // pthread wrapper (core)
  {"pthread_rwlock_rdlock", (uintptr_t)&pthread_rwlock_rdlock_fake},  // pthread wrapper (core)
  {"pthread_rwlock_unlock", (uintptr_t)&pthread_rwlock_unlock_fake},  // pthread wrapper (core)
  {"pthread_rwlock_wrlock", (uintptr_t)&pthread_rwlock_wrlock_fake},  // pthread wrapper (core)
  {"pthread_self", (uintptr_t)&pthread_self_fake},  // pthread wrapper (core)
  {"pthread_setname_np", (uintptr_t)&pthread_setname_np_fake},  // pthread wrapper (core)
  {"pthread_setschedparam", (uintptr_t)&pthread_setschedparam_fake},  // pthread wrapper (core)
  {"pthread_setspecific", (uintptr_t)&pthread_setspecific_fake},  // pthread wrapper (core)
  {"qsort", (uintptr_t)&qsort},  // pass
  {"realloc", (uintptr_t)&realloc},  // pass
  // TODO {"remove", (uintptr_t)&stub_remove},  // <<< IMPLEMENTAR
  {"rewind", (uintptr_t)&rewind},  // pass
  // TODO {"sched_get_priority_max", (uintptr_t)&stub_sched_get_priority_max},  // <<< IMPLEMENTAR
  // TODO {"sched_get_priority_min", (uintptr_t)&stub_sched_get_priority_min},  // <<< IMPLEMENTAR
  {"sem_destroy", (uintptr_t)&sem_destroy_fake},  // pthread wrapper (core)
  {"sem_init", (uintptr_t)&sem_init_fake},  // pthread wrapper (core)
  {"sem_post", (uintptr_t)&sem_post_fake},  // pthread wrapper (core)
  {"sem_wait", (uintptr_t)&sem_wait_fake},  // pthread wrapper (core)
  // TODO {"__sF", (uintptr_t)&stub___sF},  // <<< IMPLEMENTAR
  // TODO {"sincosf", (uintptr_t)&stub_sincosf},  // <<< IMPLEMENTAR
  {"sinf", (uintptr_t)&sinf},  // pass
  {"slCreateEngine", (uintptr_t)&slCreateEngine_shim},  // opensles_shim
  {"sleep", (uintptr_t)&sleep_shim},  // opensles_shim
  {"SL_IID_ANDROIDSIMPLEBUFFERQUEUE", (uintptr_t)&SL_IID_ANDROIDSIMPLEBUFFERQUEUE_shim},  // opensles_shim
  {"SL_IID_ENGINE", (uintptr_t)&SL_IID_ENGINE_shim},  // opensles_shim
  {"SL_IID_PLAY", (uintptr_t)&SL_IID_PLAY_shim},  // opensles_shim
  {"SL_IID_PLAYBACKRATE", (uintptr_t)&SL_IID_PLAYBACKRATE_shim},  // opensles_shim
  {"SL_IID_SEEK", (uintptr_t)&SL_IID_SEEK_shim},  // opensles_shim
  {"SL_IID_VOLUME", (uintptr_t)&SL_IID_VOLUME_shim},  // opensles_shim
  {"sscanf", (uintptr_t)&sscanf},  // pass
  {"__stack_chk_fail", (uintptr_t)&__stack_chk_fail},  // abi
  {"stat", (uintptr_t)&stat},  // pass
  {"strcasecmp", (uintptr_t)&strcasecmp},  // pass
  {"strcat", (uintptr_t)&strcat},  // pass
  // TODO {"__strcat_chk", (uintptr_t)&stub___strcat_chk},  // <<< IMPLEMENTAR
  {"strchr", (uintptr_t)&strchr},  // pass
  // TODO {"__strchr_chk", (uintptr_t)&stub___strchr_chk},  // <<< IMPLEMENTAR
  {"strcmp", (uintptr_t)&strcmp},  // pass
  {"strcpy", (uintptr_t)&strcpy},  // pass
  // TODO {"__strcpy_chk", (uintptr_t)&stub___strcpy_chk},  // <<< IMPLEMENTAR
  {"strerror", (uintptr_t)&strerror},  // pass
  {"strftime", (uintptr_t)&strftime},  // pass
  {"strlen", (uintptr_t)&strlen},  // pass
  // TODO {"__strlen_chk", (uintptr_t)&stub___strlen_chk},  // <<< IMPLEMENTAR
  {"strncasecmp", (uintptr_t)&strncasecmp},  // pass
  {"strncat", (uintptr_t)&strncat},  // pass
  {"strncmp", (uintptr_t)&strncmp},  // pass
  {"strncpy", (uintptr_t)&strncpy},  // pass
  // TODO {"__strncpy_chk", (uintptr_t)&stub___strncpy_chk},  // <<< IMPLEMENTAR
  // TODO {"__strncpy_chk2", (uintptr_t)&stub___strncpy_chk2},  // <<< IMPLEMENTAR
  {"strrchr", (uintptr_t)&strrchr},  // pass
  // TODO {"__strrchr_chk", (uintptr_t)&stub___strrchr_chk},  // <<< IMPLEMENTAR
  {"strstr", (uintptr_t)&strstr},  // pass
  {"strtok", (uintptr_t)&strtok},  // pass
  {"strtol", (uintptr_t)&strtol},  // pass
  {"strtoul", (uintptr_t)&strtoul},  // pass
  // TODO {"syscall", (uintptr_t)&stub_syscall},  // <<< IMPLEMENTAR
  // TODO {"syslog", (uintptr_t)&stub_syslog},  // <<< IMPLEMENTAR
  // TODO {"__system_property_get", (uintptr_t)&stub___system_property_get},  // <<< IMPLEMENTAR
  {"time", (uintptr_t)&time},  // pass
  // TODO {"ungetc", (uintptr_t)&stub_ungetc},  // <<< IMPLEMENTAR
  {"usleep", (uintptr_t)&usleep},  // pass
  // TODO {"vasprintf", (uintptr_t)&stub_vasprintf},  // <<< IMPLEMENTAR
  // TODO {"vfprintf", (uintptr_t)&stub_vfprintf},  // <<< IMPLEMENTAR
  {"vsnprintf", (uintptr_t)&vsnprintf},  // pass
  // TODO {"__vsnprintf_chk", (uintptr_t)&stub___vsnprintf_chk},  // <<< IMPLEMENTAR
  // TODO {"__vsprintf_chk", (uintptr_t)&stub___vsprintf_chk},  // <<< IMPLEMENTAR
};
const int dynlib_functions_count = sizeof(dynlib_functions)/sizeof(dynlib_functions[0]);

// ===================== SIMBOLOS A IMPLEMENTAR =====================
//   acosf
//   android_set_abort_message
//   asinf
//   atanf
//   ceill
//   closelog
//   _ctype_
//   dl_iterate_phdr
//   ferror
//   floorl
//   fmodl
//   getauxval
//   log10f
//   log10l
//   __memcpy_chk
//   __memmove_chk
//   __open_2
//   openlog
//   powl
//   remove
//   sched_get_priority_max
//   sched_get_priority_min
//   __sF
//   sincosf
//   __strcat_chk
//   __strchr_chk
//   __strcpy_chk
//   __strlen_chk
//   __strncpy_chk
//   __strncpy_chk2
//   __strrchr_chk
//   syscall
//   syslog
//   __system_property_get
//   ungetc
//   vasprintf
//   vfprintf
//   __vsnprintf_chk
//   __vsprintf_chk
