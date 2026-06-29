// imports.gen.c — GERADO por new-port.sh para 'kz_gen' (libyoyo.so)
// 325 simbolos. Resolva os UNKNOWN no fim do arquivo.
#include "imports.h"
#include "so_util.h"
#include <stdio.h>

// === passthrough/pthread/shim: ligados automaticamente ===
DynLibFunction dynlib_functions[] = {
  {"abort", (uintptr_t)&abort},  // pass
  // TODO {"accept", (uintptr_t)&stub_accept},  // <<< IMPLEMENTAR
  {"acos", (uintptr_t)&acos},  // pass
  // TODO {"acosf", (uintptr_t)&stub_acosf},  // <<< IMPLEMENTAR
  {"__android_log_print", (uintptr_t)&__android_log_print},  // liblog
  {"__android_log_vprint", (uintptr_t)&__android_log_vprint},  // liblog
  // TODO {"android_set_abort_message", (uintptr_t)&stub_android_set_abort_message},  // <<< IMPLEMENTAR
  {"asin", (uintptr_t)&asin},  // pass
  // TODO {"asinh", (uintptr_t)&stub_asinh},  // <<< IMPLEMENTAR
  // TODO {"asprintf", (uintptr_t)&stub_asprintf},  // <<< IMPLEMENTAR
  // TODO {"__assert2", (uintptr_t)&stub___assert2},  // <<< IMPLEMENTAR
  {"atan", (uintptr_t)&atan},  // pass
  {"atan2", (uintptr_t)&atan2},  // pass
  {"atan2f", (uintptr_t)&atan2f},  // pass
  {"atof", (uintptr_t)&atof},  // pass
  {"atoi", (uintptr_t)&atoi},  // pass
  {"atol", (uintptr_t)&atol},  // pass
  // TODO {"bind", (uintptr_t)&stub_bind},  // <<< IMPLEMENTAR
  {"bsearch", (uintptr_t)&bsearch},  // pass
  // TODO {"btowc", (uintptr_t)&stub_btowc},  // <<< IMPLEMENTAR
  {"calloc", (uintptr_t)&calloc},  // pass
  {"cbrt", (uintptr_t)&cbrt},  // pass
  // TODO {"clearerr", (uintptr_t)&stub_clearerr},  // <<< IMPLEMENTAR
  {"clock_gettime", (uintptr_t)&clock_gettime},  // pass
  {"close", (uintptr_t)&close},  // pass
  {"closedir", (uintptr_t)&closedir},  // pass
  // TODO {"closelog", (uintptr_t)&stub_closelog},  // <<< IMPLEMENTAR
  {"compress", (uintptr_t)&compress},  // pass
  // TODO {"connect", (uintptr_t)&stub_connect},  // <<< IMPLEMENTAR
  {"cos", (uintptr_t)&cos},  // pass
  {"cosf", (uintptr_t)&cosf},  // pass
  {"crc32", (uintptr_t)&crc32},  // pass
  // TODO {"__ctype_get_mb_cur_max", (uintptr_t)&stub___ctype_get_mb_cur_max},  // <<< IMPLEMENTAR
  {"__cxa_atexit", (uintptr_t)&__cxa_atexit},  // cxx
  {"__cxa_finalize", (uintptr_t)&__cxa_finalize},  // cxx
  {"deflate", (uintptr_t)&deflate},  // pass
  {"deflateEnd", (uintptr_t)&deflateEnd},  // pass
  {"deflateInit_", (uintptr_t)&deflateInit_},  // pass
  {"deflateInit2_", (uintptr_t)&deflateInit2_},  // pass
  // TODO {"deflateReset", (uintptr_t)&stub_deflateReset},  // <<< IMPLEMENTAR
  // TODO {"dlclose", (uintptr_t)&stub_dlclose},  // <<< IMPLEMENTAR
  // TODO {"dlerror", (uintptr_t)&stub_dlerror},  // <<< IMPLEMENTAR
  // TODO {"dl_iterate_phdr", (uintptr_t)&stub_dl_iterate_phdr},  // <<< IMPLEMENTAR
  // TODO {"dlopen", (uintptr_t)&stub_dlopen},  // <<< IMPLEMENTAR
  // TODO {"dlsym", (uintptr_t)&stub_dlsym},  // <<< IMPLEMENTAR
  {"__errno", (uintptr_t)&__errno},  // pass
  // TODO {"_exit", (uintptr_t)&stub__exit},  // <<< IMPLEMENTAR
  {"exit", (uintptr_t)&exit},  // pass
  {"exp", (uintptr_t)&exp},  // pass
  {"exp2", (uintptr_t)&exp2},  // pass
  {"expf", (uintptr_t)&expf},  // pass
  {"fclose", (uintptr_t)&fclose},  // pass
  {"fcntl", (uintptr_t)&fcntl},  // pass
  // TODO {"__FD_CLR_chk", (uintptr_t)&stub___FD_CLR_chk},  // <<< IMPLEMENTAR
  // TODO {"__FD_ISSET_chk", (uintptr_t)&stub___FD_ISSET_chk},  // <<< IMPLEMENTAR
  {"fdopen", (uintptr_t)&fdopen},  // pass
  // TODO {"__FD_SET_chk", (uintptr_t)&stub___FD_SET_chk},  // <<< IMPLEMENTAR
  // TODO {"feof", (uintptr_t)&stub_feof},  // <<< IMPLEMENTAR
  // TODO {"ferror", (uintptr_t)&stub_ferror},  // <<< IMPLEMENTAR
  {"fflush", (uintptr_t)&fflush},  // pass
  {"fgetc", (uintptr_t)&fgetc},  // pass
  // TODO {"fgetpos", (uintptr_t)&stub_fgetpos},  // <<< IMPLEMENTAR
  {"fgets", (uintptr_t)&fgets},  // pass
  {"fileno", (uintptr_t)&fileno},  // pass
  {"fmod", (uintptr_t)&fmod},  // pass
  {"fmodf", (uintptr_t)&fmodf},  // pass
  {"fopen", (uintptr_t)&fopen},  // pass
  {"fprintf", (uintptr_t)&fprintf},  // pass
  {"fputc", (uintptr_t)&fputc},  // pass
  {"fputs", (uintptr_t)&fputs},  // pass
  // TODO {"fputwc", (uintptr_t)&stub_fputwc},  // <<< IMPLEMENTAR
  {"fread", (uintptr_t)&fread},  // pass
  {"free", (uintptr_t)&free},  // pass
  // TODO {"freeaddrinfo", (uintptr_t)&stub_freeaddrinfo},  // <<< IMPLEMENTAR
  // TODO {"freelocale", (uintptr_t)&stub_freelocale},  // <<< IMPLEMENTAR
  {"fseek", (uintptr_t)&fseek},  // pass
  // TODO {"fseeko", (uintptr_t)&stub_fseeko},  // <<< IMPLEMENTAR
  {"fstat", (uintptr_t)&fstat},  // pass
  // TODO {"fstatfs", (uintptr_t)&stub_fstatfs},  // <<< IMPLEMENTAR
  // TODO {"fstatvfs", (uintptr_t)&stub_fstatvfs},  // <<< IMPLEMENTAR
  {"ftell", (uintptr_t)&ftell},  // pass
  // TODO {"ftello", (uintptr_t)&stub_ftello},  // <<< IMPLEMENTAR
  {"fwrite", (uintptr_t)&fwrite},  // pass
  // TODO {"gai_strerror", (uintptr_t)&stub_gai_strerror},  // <<< IMPLEMENTAR
  // TODO {"getaddrinfo", (uintptr_t)&stub_getaddrinfo},  // <<< IMPLEMENTAR
  // TODO {"getauxval", (uintptr_t)&stub_getauxval},  // <<< IMPLEMENTAR
  {"getc", (uintptr_t)&getc},  // pass
  {"getenv", (uintptr_t)&getenv},  // pass
  // TODO {"gethostbyname", (uintptr_t)&stub_gethostbyname},  // <<< IMPLEMENTAR
  // TODO {"getnameinfo", (uintptr_t)&stub_getnameinfo},  // <<< IMPLEMENTAR
  {"getpagesize", (uintptr_t)&getpagesize},  // pass
  // TODO {"getpeername", (uintptr_t)&stub_getpeername},  // <<< IMPLEMENTAR
  // TODO {"getpgid", (uintptr_t)&stub_getpgid},  // <<< IMPLEMENTAR
  {"getpid", (uintptr_t)&getpid},  // pass
  // TODO {"getppid", (uintptr_t)&stub_getppid},  // <<< IMPLEMENTAR
  // TODO {"getpriority", (uintptr_t)&stub_getpriority},  // <<< IMPLEMENTAR
  // TODO {"getprogname", (uintptr_t)&stub_getprogname},  // <<< IMPLEMENTAR
  // TODO {"getrusage", (uintptr_t)&stub_getrusage},  // <<< IMPLEMENTAR
  // TODO {"getsockname", (uintptr_t)&stub_getsockname},  // <<< IMPLEMENTAR
  // TODO {"getsockopt", (uintptr_t)&stub_getsockopt},  // <<< IMPLEMENTAR
  {"gettimeofday", (uintptr_t)&gettimeofday},  // pass
  // TODO {"getuid", (uintptr_t)&stub_getuid},  // <<< IMPLEMENTAR
  // TODO {"getwc", (uintptr_t)&stub_getwc},  // <<< IMPLEMENTAR
  {"gmtime", (uintptr_t)&gmtime},  // pass
  // TODO {"gmtime_r", (uintptr_t)&stub_gmtime_r},  // <<< IMPLEMENTAR
  // TODO {"if_indextoname", (uintptr_t)&stub_if_indextoname},  // <<< IMPLEMENTAR
  // TODO {"inet_ntop", (uintptr_t)&stub_inet_ntop},  // <<< IMPLEMENTAR
  // TODO {"inet_pton", (uintptr_t)&stub_inet_pton},  // <<< IMPLEMENTAR
  {"inflate", (uintptr_t)&inflate},  // pass
  {"inflateEnd", (uintptr_t)&inflateEnd},  // pass
  {"inflateInit_", (uintptr_t)&inflateInit_},  // pass
  {"inflateInit2_", (uintptr_t)&inflateInit2_},  // pass
  // TODO {"inflateReset", (uintptr_t)&stub_inflateReset},  // <<< IMPLEMENTAR
  {"ioctl", (uintptr_t)&ioctl},  // pass
  // TODO {"iswalpha_l", (uintptr_t)&stub_iswalpha_l},  // <<< IMPLEMENTAR
  // TODO {"iswblank_l", (uintptr_t)&stub_iswblank_l},  // <<< IMPLEMENTAR
  // TODO {"iswcntrl_l", (uintptr_t)&stub_iswcntrl_l},  // <<< IMPLEMENTAR
  // TODO {"iswdigit_l", (uintptr_t)&stub_iswdigit_l},  // <<< IMPLEMENTAR
  // TODO {"iswlower", (uintptr_t)&stub_iswlower},  // <<< IMPLEMENTAR
  // TODO {"iswlower_l", (uintptr_t)&stub_iswlower_l},  // <<< IMPLEMENTAR
  // TODO {"iswprint_l", (uintptr_t)&stub_iswprint_l},  // <<< IMPLEMENTAR
  // TODO {"iswpunct_l", (uintptr_t)&stub_iswpunct_l},  // <<< IMPLEMENTAR
  // TODO {"iswspace_l", (uintptr_t)&stub_iswspace_l},  // <<< IMPLEMENTAR
  // TODO {"iswupper", (uintptr_t)&stub_iswupper},  // <<< IMPLEMENTAR
  // TODO {"iswupper_l", (uintptr_t)&stub_iswupper_l},  // <<< IMPLEMENTAR
  // TODO {"iswxdigit_l", (uintptr_t)&stub_iswxdigit_l},  // <<< IMPLEMENTAR
  {"ldexp", (uintptr_t)&ldexp},  // pass
  // TODO {"listen", (uintptr_t)&stub_listen},  // <<< IMPLEMENTAR
  {"localeconv", (uintptr_t)&localeconv},  // pass
  {"localtime", (uintptr_t)&localtime},  // pass
  // TODO {"localtime_r", (uintptr_t)&stub_localtime_r},  // <<< IMPLEMENTAR
  {"log", (uintptr_t)&log},  // pass
  {"log10", (uintptr_t)&log10},  // pass
  // TODO {"log10f", (uintptr_t)&stub_log10f},  // <<< IMPLEMENTAR
  {"log2", (uintptr_t)&log2},  // pass
  {"logf", (uintptr_t)&logf},  // pass
  // TODO {"longjmp", (uintptr_t)&stub_longjmp},  // <<< IMPLEMENTAR
  {"lseek", (uintptr_t)&lseek},  // pass
  // TODO {"mallinfo", (uintptr_t)&stub_mallinfo},  // <<< IMPLEMENTAR
  {"malloc", (uintptr_t)&malloc},  // pass
  // TODO {"mbrlen", (uintptr_t)&stub_mbrlen},  // <<< IMPLEMENTAR
  // TODO {"mbrtowc", (uintptr_t)&stub_mbrtowc},  // <<< IMPLEMENTAR
  // TODO {"mbsnrtowcs", (uintptr_t)&stub_mbsnrtowcs},  // <<< IMPLEMENTAR
  // TODO {"mbsrtowcs", (uintptr_t)&stub_mbsrtowcs},  // <<< IMPLEMENTAR
  // TODO {"mbtowc", (uintptr_t)&stub_mbtowc},  // <<< IMPLEMENTAR
  {"memchr", (uintptr_t)&memchr},  // pass
  {"memcmp", (uintptr_t)&memcmp},  // pass
  {"memcpy", (uintptr_t)&memcpy},  // pass
  // TODO {"__memcpy_chk", (uintptr_t)&stub___memcpy_chk},  // <<< IMPLEMENTAR
  {"memmove", (uintptr_t)&memmove},  // pass
  // TODO {"__memmove_chk", (uintptr_t)&stub___memmove_chk},  // <<< IMPLEMENTAR
  {"memset", (uintptr_t)&memset},  // pass
  // TODO {"__memset_chk", (uintptr_t)&stub___memset_chk},  // <<< IMPLEMENTAR
  {"mkdir", (uintptr_t)&mkdir},  // pass
  // TODO {"mkstemp", (uintptr_t)&stub_mkstemp},  // <<< IMPLEMENTAR
  {"mktime", (uintptr_t)&mktime},  // pass
  {"mmap", (uintptr_t)&mmap},  // pass
  {"modf", (uintptr_t)&modf},  // pass
  // TODO {"modff", (uintptr_t)&stub_modff},  // <<< IMPLEMENTAR
  {"munmap", (uintptr_t)&munmap},  // pass
  {"nanosleep", (uintptr_t)&nanosleep},  // pass
  // TODO {"newlocale", (uintptr_t)&stub_newlocale},  // <<< IMPLEMENTAR
  {"open", (uintptr_t)&open},  // pass
  // TODO {"__open_2", (uintptr_t)&stub___open_2},  // <<< IMPLEMENTAR
  {"opendir", (uintptr_t)&opendir},  // pass
  // TODO {"openlog", (uintptr_t)&stub_openlog},  // <<< IMPLEMENTAR
  {"perror", (uintptr_t)&perror},  // pass
  {"posix_memalign", (uintptr_t)&posix_memalign},  // pass
  {"pow", (uintptr_t)&pow},  // pass
  {"powf", (uintptr_t)&powf},  // pass
  {"printf", (uintptr_t)&printf},  // pass
  {"pthread_cond_broadcast", (uintptr_t)&pthread_cond_broadcast_fake},  // pthread wrapper (core)
  {"pthread_cond_destroy", (uintptr_t)&pthread_cond_destroy_fake},  // pthread wrapper (core)
  {"pthread_cond_init", (uintptr_t)&pthread_cond_init_fake},  // pthread wrapper (core)
  {"pthread_cond_signal", (uintptr_t)&pthread_cond_signal_fake},  // pthread wrapper (core)
  {"pthread_cond_timedwait", (uintptr_t)&pthread_cond_timedwait_fake},  // pthread wrapper (core)
  {"pthread_cond_wait", (uintptr_t)&pthread_cond_wait_fake},  // pthread wrapper (core)
  {"pthread_create", (uintptr_t)&pthread_create_fake},  // pthread wrapper (core)
  {"pthread_detach", (uintptr_t)&pthread_detach_fake},  // pthread wrapper (core)
  {"pthread_equal", (uintptr_t)&pthread_equal_fake},  // pthread wrapper (core)
  {"pthread_getspecific", (uintptr_t)&pthread_getspecific_fake},  // pthread wrapper (core)
  {"pthread_join", (uintptr_t)&pthread_join_fake},  // pthread wrapper (core)
  {"pthread_key_create", (uintptr_t)&pthread_key_create_fake},  // pthread wrapper (core)
  {"pthread_key_delete", (uintptr_t)&pthread_key_delete_fake},  // pthread wrapper (core)
  {"pthread_mutexattr_destroy", (uintptr_t)&pthread_mutexattr_destroy_fake},  // pthread wrapper (core)
  {"pthread_mutexattr_init", (uintptr_t)&pthread_mutexattr_init_fake},  // pthread wrapper (core)
  {"pthread_mutexattr_settype", (uintptr_t)&pthread_mutexattr_settype_fake},  // pthread wrapper (core)
  {"pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy_fake},  // pthread wrapper (core)
  {"pthread_mutex_init", (uintptr_t)&pthread_mutex_init_fake},  // pthread wrapper (core)
  {"pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock_fake},  // pthread wrapper (core)
  {"pthread_mutex_trylock", (uintptr_t)&pthread_mutex_trylock_fake},  // pthread wrapper (core)
  {"pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_fake},  // pthread wrapper (core)
  {"pthread_once", (uintptr_t)&pthread_once_fake},  // pthread wrapper (core)
  {"pthread_rwlock_init", (uintptr_t)&pthread_rwlock_init_fake},  // pthread wrapper (core)
  {"pthread_rwlock_rdlock", (uintptr_t)&pthread_rwlock_rdlock_fake},  // pthread wrapper (core)
  {"pthread_rwlock_unlock", (uintptr_t)&pthread_rwlock_unlock_fake},  // pthread wrapper (core)
  {"pthread_rwlock_wrlock", (uintptr_t)&pthread_rwlock_wrlock_fake},  // pthread wrapper (core)
  {"pthread_self", (uintptr_t)&pthread_self_fake},  // pthread wrapper (core)
  {"pthread_setspecific", (uintptr_t)&pthread_setspecific_fake},  // pthread wrapper (core)
  {"putc", (uintptr_t)&putc},  // pass
  {"puts", (uintptr_t)&puts},  // pass
  {"qsort", (uintptr_t)&qsort},  // pass
  // TODO {"raise", (uintptr_t)&stub_raise},  // <<< IMPLEMENTAR
  {"rand", (uintptr_t)&rand},  // pass
  {"read", (uintptr_t)&read},  // pass
  // TODO {"__read_chk", (uintptr_t)&stub___read_chk},  // <<< IMPLEMENTAR
  {"readdir", (uintptr_t)&readdir},  // pass
  {"realloc", (uintptr_t)&realloc},  // pass
  // TODO {"recvfrom", (uintptr_t)&stub_recvfrom},  // <<< IMPLEMENTAR
  // TODO {"__register_atfork", (uintptr_t)&stub___register_atfork},  // <<< IMPLEMENTAR
  // TODO {"remove", (uintptr_t)&stub_remove},  // <<< IMPLEMENTAR
  {"rename", (uintptr_t)&rename},  // pass
  // TODO {"scandir", (uintptr_t)&stub_scandir},  // <<< IMPLEMENTAR
  // TODO {"select", (uintptr_t)&stub_select},  // <<< IMPLEMENTAR
  // TODO {"sendto", (uintptr_t)&stub_sendto},  // <<< IMPLEMENTAR
  // TODO {"setjmp", (uintptr_t)&stub_setjmp},  // <<< IMPLEMENTAR
  {"setlocale", (uintptr_t)&setlocale},  // pass
  // TODO {"setsockopt", (uintptr_t)&stub_setsockopt},  // <<< IMPLEMENTAR
  {"setvbuf", (uintptr_t)&setvbuf},  // pass
  // TODO {"__sF", (uintptr_t)&stub___sF},  // <<< IMPLEMENTAR
  // TODO {"shutdown", (uintptr_t)&stub_shutdown},  // <<< IMPLEMENTAR
  // TODO {"sigaction", (uintptr_t)&stub_sigaction},  // <<< IMPLEMENTAR
  // TODO {"signal", (uintptr_t)&stub_signal},  // <<< IMPLEMENTAR
  // TODO {"sigpending", (uintptr_t)&stub_sigpending},  // <<< IMPLEMENTAR
  // TODO {"sigprocmask", (uintptr_t)&stub_sigprocmask},  // <<< IMPLEMENTAR
  {"sin", (uintptr_t)&sin},  // pass
  // TODO {"sincos", (uintptr_t)&stub_sincos},  // <<< IMPLEMENTAR
  // TODO {"sincosf", (uintptr_t)&stub_sincosf},  // <<< IMPLEMENTAR
  {"sinf", (uintptr_t)&sinf},  // pass
  // TODO {"sinh", (uintptr_t)&stub_sinh},  // <<< IMPLEMENTAR
  {"snprintf", (uintptr_t)&snprintf},  // pass
  // TODO {"socket", (uintptr_t)&stub_socket},  // <<< IMPLEMENTAR
  {"srand", (uintptr_t)&srand},  // pass
  {"sscanf", (uintptr_t)&sscanf},  // pass
  {"__stack_chk_fail", (uintptr_t)&__stack_chk_fail},  // abi
  {"stat", (uintptr_t)&stat},  // pass
  // TODO {"statfs", (uintptr_t)&stub_statfs},  // <<< IMPLEMENTAR
  // TODO {"statvfs", (uintptr_t)&stub_statvfs},  // <<< IMPLEMENTAR
  {"strcat", (uintptr_t)&strcat},  // pass
  // TODO {"__strcat_chk", (uintptr_t)&stub___strcat_chk},  // <<< IMPLEMENTAR
  {"strchr", (uintptr_t)&strchr},  // pass
  // TODO {"__strchr_chk", (uintptr_t)&stub___strchr_chk},  // <<< IMPLEMENTAR
  {"strcmp", (uintptr_t)&strcmp},  // pass
  // TODO {"strcoll_l", (uintptr_t)&stub_strcoll_l},  // <<< IMPLEMENTAR
  {"strcpy", (uintptr_t)&strcpy},  // pass
  // TODO {"__strcpy_chk", (uintptr_t)&stub___strcpy_chk},  // <<< IMPLEMENTAR
  // TODO {"strcspn", (uintptr_t)&stub_strcspn},  // <<< IMPLEMENTAR
  {"strdup", (uintptr_t)&strdup},  // pass
  {"strerror", (uintptr_t)&strerror},  // pass
  // TODO {"strerror_r", (uintptr_t)&stub_strerror_r},  // <<< IMPLEMENTAR
  {"strftime", (uintptr_t)&strftime},  // pass
  // TODO {"strftime_l", (uintptr_t)&stub_strftime_l},  // <<< IMPLEMENTAR
  // TODO {"__strlcat_chk", (uintptr_t)&stub___strlcat_chk},  // <<< IMPLEMENTAR
  // TODO {"__strlcpy_chk", (uintptr_t)&stub___strlcpy_chk},  // <<< IMPLEMENTAR
  {"strlen", (uintptr_t)&strlen},  // pass
  // TODO {"__strlen_chk", (uintptr_t)&stub___strlen_chk},  // <<< IMPLEMENTAR
  {"strncat", (uintptr_t)&strncat},  // pass
  // TODO {"__strncat_chk", (uintptr_t)&stub___strncat_chk},  // <<< IMPLEMENTAR
  {"strncmp", (uintptr_t)&strncmp},  // pass
  {"strncpy", (uintptr_t)&strncpy},  // pass
  // TODO {"__strncpy_chk", (uintptr_t)&stub___strncpy_chk},  // <<< IMPLEMENTAR
  // TODO {"__strncpy_chk2", (uintptr_t)&stub___strncpy_chk2},  // <<< IMPLEMENTAR
  {"strrchr", (uintptr_t)&strrchr},  // pass
  // TODO {"__strrchr_chk", (uintptr_t)&stub___strrchr_chk},  // <<< IMPLEMENTAR
  // TODO {"strspn", (uintptr_t)&stub_strspn},  // <<< IMPLEMENTAR
  {"strstr", (uintptr_t)&strstr},  // pass
  {"strtod", (uintptr_t)&strtod},  // pass
  {"strtof", (uintptr_t)&strtof},  // pass
  {"strtok", (uintptr_t)&strtok},  // pass
  // TODO {"strtok_r", (uintptr_t)&stub_strtok_r},  // <<< IMPLEMENTAR
  {"strtol", (uintptr_t)&strtol},  // pass
  // TODO {"strtold", (uintptr_t)&stub_strtold},  // <<< IMPLEMENTAR
  // TODO {"strtold_l", (uintptr_t)&stub_strtold_l},  // <<< IMPLEMENTAR
  // TODO {"strtoll", (uintptr_t)&stub_strtoll},  // <<< IMPLEMENTAR
  // TODO {"strtoll_l", (uintptr_t)&stub_strtoll_l},  // <<< IMPLEMENTAR
  {"strtoul", (uintptr_t)&strtoul},  // pass
  // TODO {"strtoull", (uintptr_t)&stub_strtoull},  // <<< IMPLEMENTAR
  // TODO {"strtoull_l", (uintptr_t)&stub_strtoull_l},  // <<< IMPLEMENTAR
  // TODO {"strxfrm_l", (uintptr_t)&stub_strxfrm_l},  // <<< IMPLEMENTAR
  // TODO {"swprintf", (uintptr_t)&stub_swprintf},  // <<< IMPLEMENTAR
  // TODO {"syscall", (uintptr_t)&stub_syscall},  // <<< IMPLEMENTAR
  {"sysconf", (uintptr_t)&sysconf},  // pass
  // TODO {"syslog", (uintptr_t)&stub_syslog},  // <<< IMPLEMENTAR
  // TODO {"__system_property_get", (uintptr_t)&stub___system_property_get},  // <<< IMPLEMENTAR
  {"tan", (uintptr_t)&tan},  // pass
  {"tanf", (uintptr_t)&tanf},  // pass
  {"time", (uintptr_t)&time},  // pass
  // TODO {"towlower", (uintptr_t)&stub_towlower},  // <<< IMPLEMENTAR
  // TODO {"towlower_l", (uintptr_t)&stub_towlower_l},  // <<< IMPLEMENTAR
  // TODO {"towupper", (uintptr_t)&stub_towupper},  // <<< IMPLEMENTAR
  // TODO {"towupper_l", (uintptr_t)&stub_towupper_l},  // <<< IMPLEMENTAR
  // TODO {"ungetc", (uintptr_t)&stub_ungetc},  // <<< IMPLEMENTAR
  // TODO {"ungetwc", (uintptr_t)&stub_ungetwc},  // <<< IMPLEMENTAR
  // TODO {"uselocale", (uintptr_t)&stub_uselocale},  // <<< IMPLEMENTAR
  {"usleep", (uintptr_t)&usleep},  // pass
  // TODO {"vasprintf", (uintptr_t)&stub_vasprintf},  // <<< IMPLEMENTAR
  // TODO {"vfprintf", (uintptr_t)&stub_vfprintf},  // <<< IMPLEMENTAR
  // TODO {"vprintf", (uintptr_t)&stub_vprintf},  // <<< IMPLEMENTAR
  {"vsnprintf", (uintptr_t)&vsnprintf},  // pass
  // TODO {"__vsnprintf_chk", (uintptr_t)&stub___vsnprintf_chk},  // <<< IMPLEMENTAR
  // TODO {"__vsprintf_chk", (uintptr_t)&stub___vsprintf_chk},  // <<< IMPLEMENTAR
  // TODO {"vsscanf", (uintptr_t)&stub_vsscanf},  // <<< IMPLEMENTAR
  // TODO {"wcrtomb", (uintptr_t)&stub_wcrtomb},  // <<< IMPLEMENTAR
  // TODO {"wcscoll_l", (uintptr_t)&stub_wcscoll_l},  // <<< IMPLEMENTAR
  // TODO {"wcslen", (uintptr_t)&stub_wcslen},  // <<< IMPLEMENTAR
  // TODO {"wcsnrtombs", (uintptr_t)&stub_wcsnrtombs},  // <<< IMPLEMENTAR
  // TODO {"wcstod", (uintptr_t)&stub_wcstod},  // <<< IMPLEMENTAR
  // TODO {"wcstof", (uintptr_t)&stub_wcstof},  // <<< IMPLEMENTAR
  // TODO {"wcstol", (uintptr_t)&stub_wcstol},  // <<< IMPLEMENTAR
  // TODO {"wcstold", (uintptr_t)&stub_wcstold},  // <<< IMPLEMENTAR
  // TODO {"wcstoll", (uintptr_t)&stub_wcstoll},  // <<< IMPLEMENTAR
  // TODO {"wcstoul", (uintptr_t)&stub_wcstoul},  // <<< IMPLEMENTAR
  // TODO {"wcstoull", (uintptr_t)&stub_wcstoull},  // <<< IMPLEMENTAR
  // TODO {"wcsxfrm_l", (uintptr_t)&stub_wcsxfrm_l},  // <<< IMPLEMENTAR
  // TODO {"wctob", (uintptr_t)&stub_wctob},  // <<< IMPLEMENTAR
  // TODO {"wmemchr", (uintptr_t)&stub_wmemchr},  // <<< IMPLEMENTAR
  // TODO {"wmemcmp", (uintptr_t)&stub_wmemcmp},  // <<< IMPLEMENTAR
  {"write", (uintptr_t)&write},  // pass
  // TODO {"zError", (uintptr_t)&stub_zError},  // <<< IMPLEMENTAR
};
const int dynlib_functions_count = sizeof(dynlib_functions)/sizeof(dynlib_functions[0]);

// ===================== SIMBOLOS A IMPLEMENTAR =====================
//   accept
//   acosf
//   android_set_abort_message
//   asinh
//   asprintf
//   __assert2
//   bind
//   btowc
//   clearerr
//   closelog
//   connect
//   __ctype_get_mb_cur_max
//   deflateReset
//   dlclose
//   dlerror
//   dl_iterate_phdr
//   dlopen
//   dlsym
//   _exit
//   __FD_CLR_chk
//   __FD_ISSET_chk
//   __FD_SET_chk
//   feof
//   ferror
//   fgetpos
//   fputwc
//   freeaddrinfo
//   freelocale
//   fseeko
//   fstatfs
//   fstatvfs
//   ftello
//   gai_strerror
//   getaddrinfo
//   getauxval
//   gethostbyname
//   getnameinfo
//   getpeername
//   getpgid
//   getppid
//   getpriority
//   getprogname
//   getrusage
//   getsockname
//   getsockopt
//   getuid
//   getwc
//   gmtime_r
//   if_indextoname
//   inet_ntop
//   inet_pton
//   inflateReset
//   iswalpha_l
//   iswblank_l
//   iswcntrl_l
//   iswdigit_l
//   iswlower
//   iswlower_l
//   iswprint_l
//   iswpunct_l
//   iswspace_l
//   iswupper
//   iswupper_l
//   iswxdigit_l
//   listen
//   localtime_r
//   log10f
//   longjmp
//   mallinfo
//   mbrlen
//   mbrtowc
//   mbsnrtowcs
//   mbsrtowcs
//   mbtowc
//   __memcpy_chk
//   __memmove_chk
//   __memset_chk
//   mkstemp
//   modff
//   newlocale
//   __open_2
//   openlog
//   raise
//   __read_chk
//   recvfrom
//   __register_atfork
//   remove
//   scandir
//   select
//   sendto
//   setjmp
//   setsockopt
//   __sF
//   shutdown
//   sigaction
//   signal
//   sigpending
//   sigprocmask
//   sincos
//   sincosf
//   sinh
//   socket
//   statfs
//   statvfs
//   __strcat_chk
//   __strchr_chk
//   strcoll_l
//   __strcpy_chk
//   strcspn
//   strerror_r
//   strftime_l
//   __strlcat_chk
//   __strlcpy_chk
//   __strlen_chk
//   __strncat_chk
//   __strncpy_chk
//   __strncpy_chk2
//   __strrchr_chk
//   strspn
//   strtok_r
//   strtold
//   strtold_l
//   strtoll
//   strtoll_l
//   strtoull
//   strtoull_l
//   strxfrm_l
//   swprintf
//   syscall
//   syslog
//   __system_property_get
//   towlower
//   towlower_l
//   towupper
//   towupper_l
//   ungetc
//   ungetwc
//   uselocale
//   vasprintf
//   vfprintf
//   vprintf
//   __vsnprintf_chk
//   __vsprintf_chk
//   vsscanf
//   wcrtomb
//   wcscoll_l
//   wcslen
//   wcsnrtombs
//   wcstod
//   wcstof
//   wcstol
//   wcstold
//   wcstoll
//   wcstoul
//   wcstoull
//   wcsxfrm_l
//   wctob
//   wmemchr
//   wmemcmp
//   zError
