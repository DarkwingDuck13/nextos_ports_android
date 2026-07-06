// imports_unity.gen.c — GERADO. passthrough via dlsym; resto = stub log.
#include "so_util.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <string.h>

extern void *cvgos_eh_resolve_public(const char *name);

static struct { const char *name; void *p; } g_cvgos_eh_cache[32];
static int g_cvgos_eh_cache_n;

void cvgos_imports_set_eh_symbol(const char *name, void *p) {
  if (!name || !p) return;
  for (int i = 0; i < g_cvgos_eh_cache_n; i++) {
    if (!strcmp(g_cvgos_eh_cache[i].name, name)) {
      g_cvgos_eh_cache[i].p = p;
      return;
    }
  }
  if (g_cvgos_eh_cache_n < (int)(sizeof g_cvgos_eh_cache / sizeof g_cvgos_eh_cache[0])) {
    g_cvgos_eh_cache[g_cvgos_eh_cache_n].name = name;
    g_cvgos_eh_cache[g_cvgos_eh_cache_n].p = p;
    g_cvgos_eh_cache_n++;
  }
}

static void *cvgos_eh_resolve_gen(const char *name) {
  void *pub = cvgos_eh_resolve_public(name);
  if (pub) return pub;
  if (getenv("CVGOS_EHLOG")) fprintf(stderr, "[EHGEN] public miss %s\n", name ? name : "(null)");
  for (int i = 0; i < g_cvgos_eh_cache_n; i++)
    if (!strcmp(g_cvgos_eh_cache[i].name, name))
      return g_cvgos_eh_cache[i].p;
  if (getenv("CVGOS_EHLOG")) fprintf(stderr, "[EHGEN] local cache miss %s n=%d\n", name ? name : "(null)", g_cvgos_eh_cache_n);
  static void *h;
  if (!h) {
    h = dlopen("libgcc_s.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!h) h = dlopen("/usr/lib32/libgcc_s.so.1", RTLD_NOW | RTLD_GLOBAL);
  }
  void *p = h ? dlsym(h, name) : NULL;
  if (!p) p = dlsym(RTLD_DEFAULT, name);
  if (getenv("CVGOS_EHLOG")) fprintf(stderr, "[EHGEN] dlsym %s h=%p -> %p\n", name ? name : "(null)", h, p);
  return p;
}

// stubs (logam nome, 1as 2 vezes, retornam 0)
long stub_accept(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] accept\\n"); return 0; }
long stub___aeabi_atexit(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __aeabi_atexit\\n"); return 0; }
long stub___aeabi_memclr(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __aeabi_memclr\\n"); return 0; }
long stub___aeabi_memclr4(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __aeabi_memclr4\\n"); return 0; }
long stub___aeabi_memclr8(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __aeabi_memclr8\\n"); return 0; }
long stub___aeabi_memcpy(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __aeabi_memcpy\\n"); return 0; }
long stub___aeabi_memcpy4(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __aeabi_memcpy4\\n"); return 0; }
long stub___aeabi_memcpy8(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __aeabi_memcpy8\\n"); return 0; }
long stub___aeabi_memmove(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __aeabi_memmove\\n"); return 0; }
long stub___aeabi_memmove4(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __aeabi_memmove4\\n"); return 0; }
long stub___aeabi_memmove8(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __aeabi_memmove8\\n"); return 0; }
long stub___aeabi_memset(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __aeabi_memset\\n"); return 0; }
long stub___aeabi_memset4(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __aeabi_memset4\\n"); return 0; }
long stub___aeabi_memset8(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __aeabi_memset8\\n"); return 0; }
long stub___aeabi_unwind_cpp_pr0(long a, void *b, void *c){ static long (*real)(long, void *, void *); if(!real) real=(void*)cvgos_eh_resolve_gen("__aeabi_unwind_cpp_pr0"); if(real) return real(a,b,c); static int n=0; if(n++<2) fprintf(stderr,"[STUB] __aeabi_unwind_cpp_pr0\\n"); return 0; }
long stub___aeabi_unwind_cpp_pr1(long a, void *b, void *c){ static long (*real)(long, void *, void *); if(!real) real=(void*)cvgos_eh_resolve_gen("__aeabi_unwind_cpp_pr1"); if(real) return real(a,b,c); static int n=0; if(n++<2) fprintf(stderr,"[STUB] __aeabi_unwind_cpp_pr1\\n"); return 0; }
long stub_ALooper_forThread(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ALooper_forThread\\n"); return 0; }
long stub_ALooper_prepare(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ALooper_prepare\\n"); return 0; }
long stub_ANativeWindow_acquire(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ANativeWindow_acquire\\n"); return 0; }
long stub_ANativeWindow_fromSurface(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ANativeWindow_fromSurface\\n"); return 0; }
long stub_ANativeWindow_getHeight(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ANativeWindow_getHeight\\n"); return 0; }
long stub_ANativeWindow_getWidth(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ANativeWindow_getWidth\\n"); return 0; }
long stub_ANativeWindow_release(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ANativeWindow_release\\n"); return 0; }
long stub_ANativeWindow_setBuffersGeometry(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ANativeWindow_setBuffersGeometry\\n"); return 0; }
long stub___android_log_print(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __android_log_print\\n"); return 0; }
long stub___android_log_vprint(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __android_log_vprint\\n"); return 0; }
long stub___android_log_write(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __android_log_write\\n"); return 0; }
long stub_asctime(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] asctime\\n"); return 0; }
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
long stub_bind(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] bind\\n"); return 0; }
long stub_bsd_signal(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] bsd_signal\\n"); return 0; }
long stub_chmod(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] chmod\\n"); return 0; }
long stub_connect(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] connect\\n"); return 0; }
long stub__ctype_(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] _ctype_\\n"); return 0; }
long stub___cxa_atexit(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __cxa_atexit\\n"); return 0; }
long stub___cxa_begin_cleanup(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __cxa_begin_cleanup\\n"); return 0; }
long stub___cxa_call_unexpected(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __cxa_call_unexpected\\n"); return 0; }
long stub___cxa_finalize(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __cxa_finalize\\n"); return 0; }
long stub___cxa_type_match(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __cxa_type_match\\n"); return 0; }
long stub_dladdr(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] dladdr\\n"); return 0; }
long stub___end__(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __end__\\n"); return 0; }
long stub_environ(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] environ\\n"); return 0; }
long stub___errno(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __errno\\n"); return 0; }
long stub_execl(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] execl\\n"); return 0; }
long stub_exp2f(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] exp2f\\n"); return 0; }
long stub_flock(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] flock\\n"); return 0; }
long stub_fmaxf(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] fmaxf\\n"); return 0; }
long stub_fminf(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] fminf\\n"); return 0; }
long stub_fork(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] fork\\n"); return 0; }
long stub_freeaddrinfo(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] freeaddrinfo\\n"); return 0; }
long stub_fscanf(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] fscanf\\n"); return 0; }
long stub_fsync(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] fsync\\n"); return 0; }
long stub_gai_strerror(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] gai_strerror\\n"); return 0; }
long stub_getaddrinfo(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] getaddrinfo\\n"); return 0; }
long stub_gethostbyaddr(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] gethostbyaddr\\n"); return 0; }
long stub_gethostbyname(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] gethostbyname\\n"); return 0; }
long stub_gethostname(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] gethostname\\n"); return 0; }
long stub_getnameinfo(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] getnameinfo\\n"); return 0; }
long stub_getpeername(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] getpeername\\n"); return 0; }
long stub_getpriority(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] getpriority\\n"); return 0; }
long stub_getpwuid(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] getpwuid\\n"); return 0; }
long stub_getsockname(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] getsockname\\n"); return 0; }
long stub_getsockopt(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] getsockopt\\n"); return 0; }
long stub___gnu_Unwind_Find_exidx(void *a, void *b){ static long (*real)(void *, void *); if(!real) real=(void*)cvgos_eh_resolve_gen("__gnu_Unwind_Find_exidx"); if(real) return real(a,b); static int n=0; if(n++<2) fprintf(stderr,"[STUB] __gnu_Unwind_Find_exidx\\n"); return 0; }
long stub___gnu_unwind_frame(void *a, void *b){ static long (*real)(void *, void *); if(!real) real=(void*)cvgos_eh_resolve_gen("__gnu_unwind_frame"); if(real) return real(a,b); static int n=0; if(n++<2) fprintf(stderr,"[STUB] __gnu_unwind_frame\\n"); return 0; }
long stub___google_potentially_blocking_region_begin(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __google_potentially_blocking_region_begin\\n"); return 0; }
long stub___google_potentially_blocking_region_end(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __google_potentially_blocking_region_end\\n"); return 0; }
long stub_inet_addr(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] inet_addr\\n"); return 0; }
long stub_inet_aton(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] inet_aton\\n"); return 0; }
long stub_inet_ntoa(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] inet_ntoa\\n"); return 0; }
long stub_inet_ntop(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] inet_ntop\\n"); return 0; }
long stub_inet_pton(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] inet_pton\\n"); return 0; }
long stub_inflate(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] inflate\\n"); return 0; }
long stub_inflateEnd(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] inflateEnd\\n"); return 0; }
long stub_inflateInit2_(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] inflateInit2_\\n"); return 0; }
long stub_isatty(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] isatty\\n"); return 0; }
long stub_kill(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] kill\\n"); return 0; }
long stub_listen(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] listen\\n"); return 0; }
long stub_lrand48(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] lrand48\\n"); return 0; }
long stub_memalign(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] memalign\\n"); return 0; }
long stub_modff(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] modff\\n"); return 0; }
long stub_pclose(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] pclose\\n"); return 0; }
long stub_popen(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] popen\\n"); return 0; }
long stub___pthread_cleanup_pop(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __pthread_cleanup_pop\\n"); return 0; }
long stub___pthread_cleanup_push(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __pthread_cleanup_push\\n"); return 0; }
long stub_ptrace(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] ptrace\\n"); return 0; }
long stub_putchar(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] putchar\\n"); return 0; }
long stub_raise(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] raise\\n"); return 0; }
long stub_recv(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] recv\\n"); return 0; }
long stub_recvfrom(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] recvfrom\\n"); return 0; }
long stub_recvmsg(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] recvmsg\\n"); return 0; }
long stub_rintf(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] rintf\\n"); return 0; }
long stub_sched_get_priority_max(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] sched_get_priority_max\\n"); return 0; }
long stub_sched_get_priority_min(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] sched_get_priority_min\\n"); return 0; }
long stub_send(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] send\\n"); return 0; }
long stub_sendfile(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] sendfile\\n"); return 0; }
long stub_sendmsg(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] sendmsg\\n"); return 0; }
long stub_sendto(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] sendto\\n"); return 0; }
long stub_setpriority(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] setpriority\\n"); return 0; }
long stub_setsockopt(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] setsockopt\\n"); return 0; }
long stub___sF(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __sF\\n"); return 0; }
long stub_shutdown(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] shutdown\\n"); return 0; }
long stub_sigaction(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] sigaction\\n"); return 0; }
long stub_sigsuspend(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] sigsuspend\\n"); return 0; }
long stub_socket(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] socket\\n"); return 0; }
long stub___stack_chk_fail(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __stack_chk_fail\\n"); return 0; }
long stub___stack_chk_guard(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __stack_chk_guard\\n"); return 0; }
long stub_strlcpy(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] strlcpy\\n"); return 0; }
long stub_strnlen(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] strnlen\\n"); return 0; }
long stub_strsep(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] strsep\\n"); return 0; }
long stub_strxfrm(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] strxfrm\\n"); return 0; }
long stub_syscall(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] syscall\\n"); return 0; }
long stub___system_property_get(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] __system_property_get\\n"); return 0; }
long stub_tgkill(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] tgkill\\n"); return 0; }
long stub_uname(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] uname\\n"); return 0; }
long stub__Unwind_Backtrace(void *a, void *b){ (void)a; (void)b; static int n=0; if(n++<2) fprintf(stderr,"[STUB] _Unwind_Backtrace -> 0\\n"); return 0; }
long stub__Unwind_Complete(void *a){ static void (*real)(void *); if(!real) real=(void*)cvgos_eh_resolve_gen("_Unwind_Complete"); if(real) { real(a); return 0; } static int n=0; if(n++<2) fprintf(stderr,"[STUB] _Unwind_Complete\\n"); return 0; }
long stub__Unwind_DeleteException(void *a){ static void (*real)(void *); if(!real) real=(void*)cvgos_eh_resolve_gen("_Unwind_DeleteException"); if(real) { real(a); return 0; } static int n=0; if(n++<2) fprintf(stderr,"[STUB] _Unwind_DeleteException\\n"); return 0; }
long stub__Unwind_GetDataRelBase(void *a){ static long (*real)(void *); if(!real) real=(void*)cvgos_eh_resolve_gen("_Unwind_GetDataRelBase"); if(real) return real(a); static int n=0; if(n++<2) fprintf(stderr,"[STUB] _Unwind_GetDataRelBase\\n"); return 0; }
long stub__Unwind_GetLanguageSpecificData(void *a){ static long (*real)(void *); if(!real) real=(void*)cvgos_eh_resolve_gen("_Unwind_GetLanguageSpecificData"); if(real) return real(a); static int n=0; if(n++<2) fprintf(stderr,"[STUB] _Unwind_GetLanguageSpecificData\\n"); return 0; }
long stub__Unwind_GetRegionStart(void *a){ static long (*real)(void *); if(!real) real=(void*)cvgos_eh_resolve_gen("_Unwind_GetRegionStart"); if(real) return real(a); static int n=0; if(n++<2) fprintf(stderr,"[STUB] _Unwind_GetRegionStart\\n"); return 0; }
long stub__Unwind_GetTextRelBase(void *a){ static long (*real)(void *); if(!real) real=(void*)cvgos_eh_resolve_gen("_Unwind_GetTextRelBase"); if(real) return real(a); static int n=0; if(n++<2) fprintf(stderr,"[STUB] _Unwind_GetTextRelBase\\n"); return 0; }
long stub__Unwind_RaiseException(void *a){ static long (*real)(void *); if(!real) real=(void*)cvgos_eh_resolve_gen("_Unwind_RaiseException"); if(real) return real(a); static int n=0; if(n++<2) fprintf(stderr,"[STUB] _Unwind_RaiseException\\n"); return 0; }
long stub__Unwind_Resume(void *a){ static void (*real)(void *); if(!real) real=(void*)cvgos_eh_resolve_gen("_Unwind_Resume"); if(real) { real(a); return 0; } static int n=0; if(n++<2) fprintf(stderr,"[STUB] _Unwind_Resume\\n"); return 0; }
long stub__Unwind_Resume_or_Rethrow(void *a){ static long (*real)(void *); if(!real) real=(void*)cvgos_eh_resolve_gen("_Unwind_Resume_or_Rethrow"); if(real) return real(a); static int n=0; if(n++<2) fprintf(stderr,"[STUB] _Unwind_Resume_or_Rethrow\\n"); return 0; }
long stub__Unwind_VRS_Get(void *a, int b, unsigned c, int d, void *e){ static long (*real)(void *, int, unsigned, int, void *); if(!real) real=(void*)cvgos_eh_resolve_gen("_Unwind_VRS_Get"); if(real) return real(a,b,c,d,e); static int n=0; if(n++<2) fprintf(stderr,"[STUB] _Unwind_VRS_Get\\n"); return 0; }
long stub__Unwind_VRS_Set(void *a, int b, unsigned c, int d, void *e){ static long (*real)(void *, int, unsigned, int, void *); if(!real) real=(void*)cvgos_eh_resolve_gen("_Unwind_VRS_Set"); if(real) return real(a,b,c,d,e); static int n=0; if(n++<2) fprintf(stderr,"[STUB] _Unwind_VRS_Set\\n"); return 0; }
long stub_utime(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] utime\\n"); return 0; }
long stub_waitpid(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] waitpid\\n"); return 0; }
long stub_writev(void){ static int n=0; if(n++<2) fprintf(stderr,"[STUB] writev\\n"); return 0; }

// flag: 1 = passthrough (resolver via dlsym), 0 = stub ja setado
static const char *passthrough_names[] = {
  "abort",
  "access",
  "acos",
  "acosf",
  "asin",
  "asinf",
  "atan",
  "atan2",
  "atan2f",
  "atanf",
  "atoi",
  "atol",
  "bsearch",
  "btowc",
  "calloc",
  "ceil",
  "ceilf",
  "chdir",
  "clearerr",
  "clock",
  "clock_getres",
  "clock_gettime",
  "close",
  "closedir",
  "cos",
  "cosf",
  "cosh",
  "difftime",
  "div",
  "dlclose",
  "dlerror",
  "dlopen",
  "dlsym",
  "dup",
  "dup2",
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
  "eglSwapBuffers",
  "eglSwapInterval",
  "eglTerminate",
  "exit",
  "exp",
  "exp2",
  "expf",
  "fclose",
  "fcntl",
  "fdopen",
  "feof",
  "ferror",
  "fflush",
  "fgets",
  "floor",
  "floorf",
  "fmod",
  "fmodf",
  "fopen",
  "fprintf",
  "fputc",
  "fputs",
  "fread",
  "free",
  "frexp",
  "fseek",
  "fstat",
  "ftell",
  "ftruncate",
  "fwrite",
  "getc",
  "getcwd",
  "getenv",
  "getpid",
  "gettid",
  "gettimeofday",
  "getuid",
  "gmtime",
  "ioctl",
  "isalnum",
  "isalpha",
  "isspace",
  "iswctype",
  "ldexp",
  "localtime",
  "log",
  "log10",
  "log10f",
  "logf",
  "longjmp",
  "lseek",
  "lseek64",
  "lstat",
  "malloc",
  "mbrtowc",
  "memchr",
  "memcmp",
  "memcpy",
  "memmem",
  "memmove",
  "memset",
  "mkdir",
  "mktime",
  "mmap",
  "modf",
  "mprotect",
  "msync",
  "munmap",
  "nanosleep",
  "open",
  "opendir",
  "perror",
  "pipe",
  "poll",
  "pow",
  "powf",
  "prctl",
  "printf",
  "pthread_attr_destroy",
  "pthread_attr_getdetachstate",
  "pthread_attr_getstack",
  "pthread_attr_init",
  "pthread_attr_setdetachstate",
  "pthread_attr_setstacksize",
  "pthread_cond_broadcast",
  "pthread_cond_destroy",
  "pthread_cond_init",
  "pthread_cond_signal",
  "pthread_cond_timedwait",
  "pthread_cond_timedwait_relative_np",
  "pthread_cond_wait",
  "pthread_create",
  "pthread_detach",
  "pthread_equal",
  "pthread_exit",
  "pthread_getattr_np",
  "pthread_getcpuclockid",
  "pthread_getschedparam",
  "pthread_getspecific",
  "pthread_join",
  "pthread_key_create",
  "pthread_key_delete",
  "pthread_kill",
  "pthread_mutexattr_destroy",
  "pthread_mutexattr_init",
  "pthread_mutexattr_settype",
  "pthread_mutex_destroy",
  "pthread_mutex_init",
  "pthread_mutex_lock",
  "pthread_mutex_trylock",
  "pthread_mutex_unlock",
  "pthread_once",
  "pthread_rwlock_destroy",
  "pthread_rwlock_init",
  "pthread_rwlock_rdlock",
  "pthread_rwlock_unlock",
  "pthread_rwlock_wrlock",
  "pthread_self",
  "pthread_setname_np",
  "pthread_setschedparam",
  "pthread_setspecific",
  "pthread_sigmask",
  "puts",
  "qsort",
  "read",
  "readdir",
  "readlink",
  "realloc",
  "realpath",
  "remainder",
  "remove",
  "rename",
  "rmdir",
  "roundf",
  "sched_yield",
  "select",
  "sem_destroy",
  "sem_getvalue",
  "sem_init",
  "sem_open",
  "sem_post",
  "sem_timedwait",
  "sem_wait",
  "setenv",
  "setjmp",
  "setlocale",
  "setvbuf",
  "siglongjmp",
  "sigsetjmp",
  "sin",
  "sinf",
  "sinh",
  "snprintf",
  "sprintf",
  "sqrt",
  "sqrtf",
  "sscanf",
  "stat",
  "statfs",
  "strcasecmp",
  "strcat",
  "strchr",
  "strcmp",
  "strcoll",
  "strcpy",
  "strdup",
  "strerror",
  "strftime",
  "strlen",
  "strncasecmp",
  "strncat",
  "strncmp",
  "strncpy",
  "strpbrk",
  "strrchr",
  "strstr",
  "strtod",
  "strtol",
  "strtoul",
  "strtoull",
  "sysconf",
  "tan",
  "tanf",
  "tanh",
  "time",
  "tolower",
  "towlower",
  "towupper",
  "truncate",
  "truncf",
  "unlink",
  "unsetenv",
  "usleep",
  "vfprintf",
  "vprintf",
  "vsnprintf",
  "vsprintf",
  "wcrtomb",
  "wcscoll",
  "wcsftime",
  "wcslen",
  "wcsxfrm",
  "wctob",
  "wctype",
  "wmemchr",
  "wmemcmp",
  "wmemcpy",
  "wmemmove",
  "wmemset",
  "write",
  0 };

DynLibFunction dynlib_functions[] = {
  {"abort", 0},
  {"accept", (uintptr_t)&stub_accept},
  {"access", 0},
  {"acos", 0},
  {"acosf", 0},
  {"__aeabi_atexit", (uintptr_t)&stub___aeabi_atexit},
  {"__aeabi_memclr", (uintptr_t)&stub___aeabi_memclr},
  {"__aeabi_memclr4", (uintptr_t)&stub___aeabi_memclr4},
  {"__aeabi_memclr8", (uintptr_t)&stub___aeabi_memclr8},
  {"__aeabi_memcpy", (uintptr_t)&stub___aeabi_memcpy},
  {"__aeabi_memcpy4", (uintptr_t)&stub___aeabi_memcpy4},
  {"__aeabi_memcpy8", (uintptr_t)&stub___aeabi_memcpy8},
  {"__aeabi_memmove", (uintptr_t)&stub___aeabi_memmove},
  {"__aeabi_memmove4", (uintptr_t)&stub___aeabi_memmove4},
  {"__aeabi_memmove8", (uintptr_t)&stub___aeabi_memmove8},
  {"__aeabi_memset", (uintptr_t)&stub___aeabi_memset},
  {"__aeabi_memset4", (uintptr_t)&stub___aeabi_memset4},
  {"__aeabi_memset8", (uintptr_t)&stub___aeabi_memset8},
  {"__aeabi_unwind_cpp_pr0", (uintptr_t)&stub___aeabi_unwind_cpp_pr0},
  {"__aeabi_unwind_cpp_pr1", (uintptr_t)&stub___aeabi_unwind_cpp_pr1},
  {"ALooper_forThread", (uintptr_t)&stub_ALooper_forThread},
  {"ALooper_prepare", (uintptr_t)&stub_ALooper_prepare},
  {"ANativeWindow_acquire", (uintptr_t)&stub_ANativeWindow_acquire},
  {"ANativeWindow_fromSurface", (uintptr_t)&stub_ANativeWindow_fromSurface},
  {"ANativeWindow_getHeight", (uintptr_t)&stub_ANativeWindow_getHeight},
  {"ANativeWindow_getWidth", (uintptr_t)&stub_ANativeWindow_getWidth},
  {"ANativeWindow_release", (uintptr_t)&stub_ANativeWindow_release},
  {"ANativeWindow_setBuffersGeometry", (uintptr_t)&stub_ANativeWindow_setBuffersGeometry},
  {"__android_log_print", (uintptr_t)&stub___android_log_print},
  {"__android_log_vprint", (uintptr_t)&stub___android_log_vprint},
  {"__android_log_write", (uintptr_t)&stub___android_log_write},
  {"asctime", (uintptr_t)&stub_asctime},
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
  {"atoi", 0},
  {"atol", 0},
  {"bind", (uintptr_t)&stub_bind},
  {"bsd_signal", (uintptr_t)&stub_bsd_signal},
  {"bsearch", 0},
  {"btowc", 0},
  {"calloc", 0},
  {"ceil", 0},
  {"ceilf", 0},
  {"chdir", 0},
  {"chmod", (uintptr_t)&stub_chmod},
  {"clearerr", 0},
  {"clock", 0},
  {"clock_getres", 0},
  {"clock_gettime", 0},
  {"close", 0},
  {"closedir", 0},
  {"connect", (uintptr_t)&stub_connect},
  {"cos", 0},
  {"cosf", 0},
  {"cosh", 0},
  {"_ctype_", (uintptr_t)&stub__ctype_},
  {"__cxa_atexit", (uintptr_t)&stub___cxa_atexit},
  {"__cxa_begin_cleanup", (uintptr_t)&stub___cxa_begin_cleanup},
  {"__cxa_call_unexpected", (uintptr_t)&stub___cxa_call_unexpected},
  {"__cxa_finalize", (uintptr_t)&stub___cxa_finalize},
  {"__cxa_type_match", (uintptr_t)&stub___cxa_type_match},
  {"difftime", 0},
  {"div", 0},
  {"dladdr", (uintptr_t)&stub_dladdr},
  {"dlclose", 0},
  {"dlerror", 0},
  {"dlopen", 0},
  {"dlsym", 0},
  {"dup", 0},
  {"dup2", 0},
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
  {"eglSwapBuffers", 0},
  {"eglSwapInterval", 0},
  {"eglTerminate", 0},
  {"__end__", (uintptr_t)&stub___end__},
  {"environ", (uintptr_t)&stub_environ},
  {"__errno", (uintptr_t)&stub___errno},
  {"execl", (uintptr_t)&stub_execl},
  {"exit", 0},
  {"exp", 0},
  {"exp2", 0},
  {"exp2f", (uintptr_t)&stub_exp2f},
  {"expf", 0},
  {"fclose", 0},
  {"fcntl", 0},
  {"fdopen", 0},
  {"feof", 0},
  {"ferror", 0},
  {"fflush", 0},
  {"fgets", 0},
  {"flock", (uintptr_t)&stub_flock},
  {"floor", 0},
  {"floorf", 0},
  {"fmaxf", (uintptr_t)&stub_fmaxf},
  {"fminf", (uintptr_t)&stub_fminf},
  {"fmod", 0},
  {"fmodf", 0},
  {"fopen", 0},
  {"fork", (uintptr_t)&stub_fork},
  {"fprintf", 0},
  {"fputc", 0},
  {"fputs", 0},
  {"fread", 0},
  {"free", 0},
  {"freeaddrinfo", (uintptr_t)&stub_freeaddrinfo},
  {"frexp", 0},
  {"fscanf", (uintptr_t)&stub_fscanf},
  {"fseek", 0},
  {"fstat", 0},
  {"fsync", (uintptr_t)&stub_fsync},
  {"ftell", 0},
  {"ftruncate", 0},
  {"fwrite", 0},
  {"gai_strerror", (uintptr_t)&stub_gai_strerror},
  {"getaddrinfo", (uintptr_t)&stub_getaddrinfo},
  {"getc", 0},
  {"getcwd", 0},
  {"getenv", 0},
  {"gethostbyaddr", (uintptr_t)&stub_gethostbyaddr},
  {"gethostbyname", (uintptr_t)&stub_gethostbyname},
  {"gethostname", (uintptr_t)&stub_gethostname},
  {"getnameinfo", (uintptr_t)&stub_getnameinfo},
  {"getpeername", (uintptr_t)&stub_getpeername},
  {"getpid", 0},
  {"getpriority", (uintptr_t)&stub_getpriority},
  {"getpwuid", (uintptr_t)&stub_getpwuid},
  {"getsockname", (uintptr_t)&stub_getsockname},
  {"getsockopt", (uintptr_t)&stub_getsockopt},
  {"gettid", 0},
  {"gettimeofday", 0},
  {"getuid", 0},
  {"gmtime", 0},
  {"__gnu_Unwind_Find_exidx", (uintptr_t)&stub___gnu_Unwind_Find_exidx},
  {"__gnu_unwind_frame", (uintptr_t)&stub___gnu_unwind_frame},
  {"__google_potentially_blocking_region_begin", (uintptr_t)&stub___google_potentially_blocking_region_begin},
  {"__google_potentially_blocking_region_end", (uintptr_t)&stub___google_potentially_blocking_region_end},
  {"inet_addr", (uintptr_t)&stub_inet_addr},
  {"inet_aton", (uintptr_t)&stub_inet_aton},
  {"inet_ntoa", (uintptr_t)&stub_inet_ntoa},
  {"inet_ntop", (uintptr_t)&stub_inet_ntop},
  {"inet_pton", (uintptr_t)&stub_inet_pton},
  {"inflate", (uintptr_t)&stub_inflate},
  {"inflateEnd", (uintptr_t)&stub_inflateEnd},
  {"inflateInit2_", (uintptr_t)&stub_inflateInit2_},
  {"ioctl", 0},
  {"isalnum", 0},
  {"isalpha", 0},
  {"isatty", (uintptr_t)&stub_isatty},
  {"isspace", 0},
  {"iswctype", 0},
  {"kill", (uintptr_t)&stub_kill},
  {"ldexp", 0},
  {"listen", (uintptr_t)&stub_listen},
  {"localtime", 0},
  {"log", 0},
  {"log10", 0},
  {"log10f", 0},
  {"logf", 0},
  {"longjmp", 0},
  {"lrand48", (uintptr_t)&stub_lrand48},
  {"lseek", 0},
  {"lseek64", 0},
  {"lstat", 0},
  {"malloc", 0},
  {"mbrtowc", 0},
  {"memalign", (uintptr_t)&stub_memalign},
  {"memchr", 0},
  {"memcmp", 0},
  {"memcpy", 0},
  {"memmem", 0},
  {"memmove", 0},
  {"memset", 0},
  {"mkdir", 0},
  {"mktime", 0},
  {"mmap", 0},
  {"modf", 0},
  {"modff", (uintptr_t)&stub_modff},
  {"mprotect", 0},
  {"msync", 0},
  {"munmap", 0},
  {"nanosleep", 0},
  {"open", 0},
  {"opendir", 0},
  {"pclose", (uintptr_t)&stub_pclose},
  {"perror", 0},
  {"pipe", 0},
  {"poll", 0},
  {"popen", (uintptr_t)&stub_popen},
  {"pow", 0},
  {"powf", 0},
  {"prctl", 0},
  {"printf", 0},
  {"pthread_attr_destroy", 0},
  {"pthread_attr_getdetachstate", 0},
  {"pthread_attr_getstack", 0},
  {"pthread_attr_init", 0},
  {"pthread_attr_setdetachstate", 0},
  {"pthread_attr_setstacksize", 0},
  {"__pthread_cleanup_pop", (uintptr_t)&stub___pthread_cleanup_pop},
  {"__pthread_cleanup_push", (uintptr_t)&stub___pthread_cleanup_push},
  {"pthread_cond_broadcast", 0},
  {"pthread_cond_destroy", 0},
  {"pthread_cond_init", 0},
  {"pthread_cond_signal", 0},
  {"pthread_cond_timedwait", 0},
  {"pthread_cond_timedwait_relative_np", 0},
  {"pthread_cond_wait", 0},
  {"pthread_create", 0},
  {"pthread_detach", 0},
  {"pthread_equal", 0},
  {"pthread_exit", 0},
  {"pthread_getattr_np", 0},
  {"pthread_getcpuclockid", 0},
  {"pthread_getschedparam", 0},
  {"pthread_getspecific", 0},
  {"pthread_join", 0},
  {"pthread_key_create", 0},
  {"pthread_key_delete", 0},
  {"pthread_kill", 0},
  {"pthread_mutexattr_destroy", 0},
  {"pthread_mutexattr_init", 0},
  {"pthread_mutexattr_settype", 0},
  {"pthread_mutex_destroy", 0},
  {"pthread_mutex_init", 0},
  {"pthread_mutex_lock", 0},
  {"pthread_mutex_trylock", 0},
  {"pthread_mutex_unlock", 0},
  {"pthread_once", 0},
  {"pthread_rwlock_destroy", 0},
  {"pthread_rwlock_init", 0},
  {"pthread_rwlock_rdlock", 0},
  {"pthread_rwlock_unlock", 0},
  {"pthread_rwlock_wrlock", 0},
  {"pthread_self", 0},
  {"pthread_setname_np", 0},
  {"pthread_setschedparam", 0},
  {"pthread_setspecific", 0},
  {"pthread_sigmask", 0},
  {"ptrace", (uintptr_t)&stub_ptrace},
  {"putchar", (uintptr_t)&stub_putchar},
  {"puts", 0},
  {"qsort", 0},
  {"raise", (uintptr_t)&stub_raise},
  {"read", 0},
  {"readdir", 0},
  {"readlink", 0},
  {"realloc", 0},
  {"realpath", 0},
  {"recv", (uintptr_t)&stub_recv},
  {"recvfrom", (uintptr_t)&stub_recvfrom},
  {"recvmsg", (uintptr_t)&stub_recvmsg},
  {"remainder", 0},
  {"remove", 0},
  {"rename", 0},
  {"rintf", (uintptr_t)&stub_rintf},
  {"rmdir", 0},
  {"roundf", 0},
  {"sched_get_priority_max", (uintptr_t)&stub_sched_get_priority_max},
  {"sched_get_priority_min", (uintptr_t)&stub_sched_get_priority_min},
  {"sched_yield", 0},
  {"select", 0},
  {"sem_destroy", 0},
  {"sem_getvalue", 0},
  {"sem_init", 0},
  {"sem_open", 0},
  {"sem_post", 0},
  {"sem_timedwait", 0},
  {"sem_wait", 0},
  {"send", (uintptr_t)&stub_send},
  {"sendfile", (uintptr_t)&stub_sendfile},
  {"sendmsg", (uintptr_t)&stub_sendmsg},
  {"sendto", (uintptr_t)&stub_sendto},
  {"setenv", 0},
  {"setjmp", 0},
  {"setlocale", 0},
  {"setpriority", (uintptr_t)&stub_setpriority},
  {"setsockopt", (uintptr_t)&stub_setsockopt},
  {"setvbuf", 0},
  {"__sF", (uintptr_t)&stub___sF},
  {"shutdown", (uintptr_t)&stub_shutdown},
  {"sigaction", (uintptr_t)&stub_sigaction},
  {"siglongjmp", 0},
  {"sigsetjmp", 0},
  {"sigsuspend", (uintptr_t)&stub_sigsuspend},
  {"sin", 0},
  {"sinf", 0},
  {"sinh", 0},
  {"snprintf", 0},
  {"socket", (uintptr_t)&stub_socket},
  {"sprintf", 0},
  {"sqrt", 0},
  {"sqrtf", 0},
  {"sscanf", 0},
  {"__stack_chk_fail", (uintptr_t)&stub___stack_chk_fail},
  {"__stack_chk_guard", (uintptr_t)&stub___stack_chk_guard},
  {"stat", 0},
  {"statfs", 0},
  {"strcasecmp", 0},
  {"strcat", 0},
  {"strchr", 0},
  {"strcmp", 0},
  {"strcoll", 0},
  {"strcpy", 0},
  {"strdup", 0},
  {"strerror", 0},
  {"strftime", 0},
  {"strlcpy", (uintptr_t)&stub_strlcpy},
  {"strlen", 0},
  {"strncasecmp", 0},
  {"strncat", 0},
  {"strncmp", 0},
  {"strncpy", 0},
  {"strnlen", (uintptr_t)&stub_strnlen},
  {"strpbrk", 0},
  {"strrchr", 0},
  {"strsep", (uintptr_t)&stub_strsep},
  {"strstr", 0},
  {"strtod", 0},
  {"strtol", 0},
  {"strtoul", 0},
  {"strtoull", 0},
  {"strxfrm", (uintptr_t)&stub_strxfrm},
  {"syscall", (uintptr_t)&stub_syscall},
  {"sysconf", 0},
  {"__system_property_get", (uintptr_t)&stub___system_property_get},
  {"tan", 0},
  {"tanf", 0},
  {"tanh", 0},
  {"tgkill", (uintptr_t)&stub_tgkill},
  {"time", 0},
  {"tolower", 0},
  {"towlower", 0},
  {"towupper", 0},
  {"truncate", 0},
  {"truncf", 0},
  {"uname", (uintptr_t)&stub_uname},
  {"unlink", 0},
  {"unsetenv", 0},
  {"_Unwind_Backtrace", (uintptr_t)&stub__Unwind_Backtrace},
  {"_Unwind_Complete", (uintptr_t)&stub__Unwind_Complete},
  {"_Unwind_DeleteException", (uintptr_t)&stub__Unwind_DeleteException},
  {"_Unwind_GetDataRelBase", (uintptr_t)&stub__Unwind_GetDataRelBase},
  {"_Unwind_GetLanguageSpecificData", (uintptr_t)&stub__Unwind_GetLanguageSpecificData},
  {"_Unwind_GetRegionStart", (uintptr_t)&stub__Unwind_GetRegionStart},
  {"_Unwind_GetTextRelBase", (uintptr_t)&stub__Unwind_GetTextRelBase},
  {"_Unwind_RaiseException", (uintptr_t)&stub__Unwind_RaiseException},
  {"_Unwind_Resume", (uintptr_t)&stub__Unwind_Resume},
  {"_Unwind_Resume_or_Rethrow", (uintptr_t)&stub__Unwind_Resume_or_Rethrow},
  {"_Unwind_VRS_Get", (uintptr_t)&stub__Unwind_VRS_Get},
  {"_Unwind_VRS_Set", (uintptr_t)&stub__Unwind_VRS_Set},
  {"usleep", 0},
  {"utime", (uintptr_t)&stub_utime},
  {"vfprintf", 0},
  {"vprintf", 0},
  {"vsnprintf", 0},
  {"vsprintf", 0},
  {"waitpid", (uintptr_t)&stub_waitpid},
  {"wcrtomb", 0},
  {"wcscoll", 0},
  {"wcsftime", 0},
  {"wcslen", 0},
  {"wcsxfrm", 0},
  {"wctob", 0},
  {"wctype", 0},
  {"wmemchr", 0},
  {"wmemcmp", 0},
  {"wmemcpy", 0},
  {"wmemmove", 0},
  {"wmemset", 0},
  {"write", 0},
  {"writev", (uintptr_t)&stub_writev},
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
