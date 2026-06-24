#define _GNU_SOURCE
// re4_imports.c -- GERADO. egl->egl_shim_*, resto->dlsym(RTLD_DEFAULT) em runtime + shims bionic.
#include "imports.h"
#include "so_util.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <dlfcn.h>
#include <time.h>
#include <pthread.h>
#include <errno.h>
#include <unistd.h>
#include <sys/sysinfo.h>
#include <string.h>
extern void egl_shim_ChooseConfig(void);
extern void egl_shim_CreateContext(void);
extern void egl_shim_CreatePbufferSurface(void);
extern void egl_shim_CreateWindowSurface(void);
extern void egl_shim_DestroyContext(void);
extern void egl_shim_DestroySurface(void);
extern void egl_shim_GetConfigAttrib(void);
extern void egl_shim_GetCurrentContext(void);
extern void egl_shim_GetCurrentSurface(void);
extern void egl_shim_GetDisplay(void);
extern void egl_shim_GetError(void);
extern void egl_shim_GetProcAddress(void);
extern void egl_shim_Initialize(void);
extern void egl_shim_MakeCurrent(void);
extern void egl_shim_QueryString(void);
extern void egl_shim_QuerySurface(void);
extern void egl_shim_SwapBuffers(void);
extern void egl_shim_SwapInterval(void);
extern void egl_shim_Terminate(void);

static long stub_generic(void){ return 0; }
/* shims bionic */
static unsigned char g_ctype[384];
static short g_tolower[384], g_toupper[384];
/* bionic _ctype_/_tolower_tab_/_toupper_tab_ sao VARIAVEIS PONTEIRO (const char*), acessadas com
   2 derefs: *(_ctype_) aponta p/ a tabela; o codigo faz tabela[c+1]. Resolver o SIMBOLO p/ o
   endereco da tabela (1 deref) crashava (libueva deref a tabela como se fosse o ponteiro). */
static const unsigned char *g_ctype_p;
static const short *g_tolower_p, *g_toupper_p;
static int g_ctype_init=0;
#include <ctype.h>
static void ctype_build(void){
  if(g_ctype_init) return; g_ctype_init=1;
  for(int c=0;c<256;c++){ unsigned char b=0;
    if(isupper(c))b|=0x01; if(islower(c))b|=0x02; if(isdigit(c))b|=0x04;
    if(isspace(c))b|=0x08; if(ispunct(c))b|=0x10; if(iscntrl(c))b|=0x20;
    if(isxdigit(c))b|=0x40; if(c==' ')b|=0x80;
    g_ctype[c+1]=b; g_tolower[c+1]=tolower(c); g_toupper[c+1]=toupper(c);
  }
}
/* pthread_cond_timedwait_relative_np (bionic): relativo -> absoluto */
static int cond_timedwait_rel(pthread_cond_t*c,pthread_mutex_t*m,const struct timespec*rel){
  if(!rel) return pthread_cond_wait(c,m);
  struct timespec abs; clock_gettime(CLOCK_REALTIME,&abs);
  abs.tv_sec+=rel->tv_sec; abs.tv_nsec+=rel->tv_nsec;
  if(abs.tv_nsec>=1000000000L){abs.tv_sec++;abs.tv_nsec-=1000000000L;}
  return pthread_cond_timedwait(c,m,&abs);
}
static long noop(void){ return 0; }
/* resolve um simbolo: bionic-shim > alias > dlsym > stub */
static long ig_sysconf(int name){ long r=sysconf(name);
  static int sc=0; if(sc++<40) fprintf(stderr,"[SYSCONF] name=%d -> %ld\n",name,r);
  if(name==_SC_PAGESIZE && r!=4096){ fprintf(stderr,"[SYSCONF] PAGESIZE %ld -> forco 4096\n",r); return 4096; }
  if((name==_SC_PHYS_PAGES||name==_SC_AVPHYS_PAGES)&&r<=0){ r=(512L*1024*1024)/4096; }
  return r; }
static int ig_getpagesize(void){ return 4096; }
/* GC pega bounds da pilha; glibc retorna base=0 p/ a thread Unity -> GC varre de ~0 -> crash.
   Forcamos bounds VALIDOS (reais via glibc, ou fallback baseado no SP atual). */
static int ig_getattr_np(void *thr,void *battr){ (void)thr;
  void *b=0; size_t sz=0; pthread_attr_t ga;
  if(pthread_getattr_np(pthread_self(),&ga)==0){ pthread_attr_getstack(&ga,&b,&sz); pthread_attr_destroy(&ga); }
  uintptr_t sp; __asm__ volatile("mov %0, sp":"=r"(sp)); uintptr_t lo=(uintptr_t)b,hi=lo+sz;
  if(!b||!sz||sp<=lo||sp>=hi){ lo=(sp&~0xfffUL)-0x80000UL; hi=(sp&~0xfffUL)+0x80000UL; b=(void*)lo; sz=hi-lo; }
  /* BIONIC pthread_attr_t armeabi-v7a = 24 bytes (flags@0,base@4,size@8,guard@12,policy@16,prio@20).
     memset de 64 ESTOURAVA o buffer do caller (24B) -> zerava os regs salvos (r4/fp/lr) na pilha
     -> F fazia pop {r4,fp,pc} com lr=0 -> CRASH pc=0 ("NULL-call no init"). Fix: 24 bytes. */
  if(battr){ memset(battr,0,24); *(void**)((char*)battr+4)=b; *(size_t*)((char*)battr+8)=sz; }
  static int n=0; if(n++<8) fprintf(stderr,"[IG-GETATTR] base=%p size=%zu sp=%p\n",b,sz,(void*)sp); return 0; }
static int ig_attr_getstack(void *attr,void **base,size_t *size){ (void)attr;
  void *b=0; size_t sz=0; pthread_attr_t ga;
  if(pthread_getattr_np(pthread_self(),&ga)==0){ pthread_attr_getstack(&ga,&b,&sz); pthread_attr_destroy(&ga); }
  uintptr_t sp; __asm__ volatile("mov %0, sp":"=r"(sp));
  uintptr_t lo=(uintptr_t)b, hi=lo+sz;
  /* o Mono (threads.c:928) exige lo < SP < hi. Se os bounds reais nao contem o SP, deriva do SP. */
  if(!b||!sz||sp<=lo||sp>=hi){ lo=(sp & ~0xfffUL)-0x80000UL; hi=(sp & ~0xfffUL)+0x80000UL; b=(void*)lo; sz=hi-lo; }
  if(base)*base=b; if(size)*size=sz;
  static int n=0; if(n++<8) fprintf(stderr,"[IG-STACK] base=%p size=%zu sp=%p\n",b,sz,(void*)sp); return 0; }
static int ig_sysinfo(struct sysinfo *info){ if(info){ memset(info,0,sizeof(*info));
  info->totalram=512UL*1024*1024; info->freeram=256UL*1024*1024; info->sharedram=0; info->bufferram=0;
  info->totalswap=256UL*1024*1024; info->freeswap=256UL*1024*1024; info->mem_unit=1; info->procs=4; info->uptime=120;
  info->loads[0]=info->loads[1]=info->loads[2]=0; } fprintf(stderr,"[SYSINFO] stub 512MB\n"); return 0; }
void *re4_resolve(const char *nm){
  if(!strcmp(nm,"sysconf")) return (void*)&ig_sysconf;
  if(!strcmp(nm,"getpagesize")) return (void*)&ig_getpagesize;
  if(!strcmp(nm,"sysinfo")) return (void*)&ig_sysinfo;
  if(!strcmp(nm,"pthread_attr_getstack")) return (void*)&ig_attr_getstack;
  if(!strcmp(nm,"pthread_getattr_np")) return (void*)&ig_getattr_np;
  ctype_build();
  /* devolve o ENDERECO da variavel ponteiro (que aponta p/ a base da tabela; codigo faz +1+c) */
  g_ctype_p=g_ctype; g_tolower_p=g_tolower; g_toupper_p=g_toupper;
  if(!strcmp(nm,"_ctype_")) return &g_ctype_p;
  if(!strcmp(nm,"_tolower_tab_")) return &g_tolower_p;
  if(!strcmp(nm,"_toupper_tab_")) return &g_toupper_p;
  if(!strcmp(nm,"__errno")) return dlsym(RTLD_DEFAULT,"__errno_location");
  if(!strcmp(nm,"setjmp")) return dlsym(RTLD_DEFAULT,"_setjmp");
  if(!strcmp(nm,"longjmp")) return dlsym(RTLD_DEFAULT,"_longjmp");
  if(!strcmp(nm,"__assert2")) return dlsym(RTLD_DEFAULT,"__assert_fail");
  if(!strcmp(nm,"pthread_cond_timedwait_relative_np")) return (void*)&cond_timedwait_rel;
  if(!strncmp(nm,"__google_potentially",20)) return (void*)&noop;
  if(!strcmp(nm,"ptrace")) return (void*)&noop;
  /* __sF: bionic stdin/stdout/stderr table. Must be our own region + the stdio
     funcs below redirect &__sF[i] to the real glibc streams, else the engine's
     fprintf/fwrite on a garbage FILE* does WILD HEAP WRITES (the corruption). */
  { extern void *dt_sF_table(void); if(!strcmp(nm,"__sF")) return dt_sF_table(); }
  { extern int dt_fprintf(); extern int dt_vfprintf(); extern unsigned dt_fwrite();
    extern unsigned dt_fread(); extern int dt_fputs(); extern int dt_fputc();
    extern int dt_putc(); extern int dt_fflush(); extern int dt_fileno();
    extern int dt_ferror(); extern int dt_feof(); extern void dt_clearerr();
    extern int dt_getc(); extern char *dt_fgets(); extern int dt_setvbuf();
    extern int dt_fseek(); extern long dt_ftell(); extern int dt_fclose();
    if(!strcmp(nm,"fprintf")) return (void*)dt_fprintf;
    if(!strcmp(nm,"vfprintf")) return (void*)dt_vfprintf;
    if(!strcmp(nm,"fwrite")) return (void*)dt_fwrite;
    if(!strcmp(nm,"fread")) return (void*)dt_fread;
    if(!strcmp(nm,"fputs")) return (void*)dt_fputs;
    if(!strcmp(nm,"fputc")) return (void*)dt_fputc;
    if(!strcmp(nm,"putc")) return (void*)dt_putc;
    if(!strcmp(nm,"fflush")) return (void*)dt_fflush;
    if(!strcmp(nm,"fileno")) return (void*)dt_fileno;
    if(!strcmp(nm,"ferror")) return (void*)dt_ferror;
    if(!strcmp(nm,"feof")) return (void*)dt_feof;
    if(!strcmp(nm,"clearerr")) return (void*)dt_clearerr;
    if(!strcmp(nm,"getc")) return (void*)dt_getc;
    if(!strcmp(nm,"fgets")) return (void*)dt_fgets;
    if(!strcmp(nm,"setvbuf")) return (void*)dt_setvbuf;
    if(!strcmp(nm,"fseek")) return (void*)dt_fseek;
    if(!strcmp(nm,"ftell")) return (void*)dt_ftell;
    if(!strcmp(nm,"fclose")) return (void*)dt_fclose; }
  if(!strcmp(nm,"bsd_signal")||!strcmp(nm,"sysv_signal")) return dlsym(RTLD_DEFAULT,"signal");
  if(!strcmp(nm,"sigignore")) return (void*)&noop;
  { extern void my_exit(int); if(!strcmp(nm,"exit")||!strcmp(nm,"_exit")||!strcmp(nm,"_Exit")) return (void*)my_exit; }
  /* ABI float: libunity/libmono sao SOFTFP, glibc libm/libc e HARDFP -> wrappers
     pcs(aapcs) (softfp_shim.c). ANTES do dlsym (que devolveria a func hardfp crua). */
  { extern void *softfp_resolve(const char *); void *sf=softfp_resolve(nm); if(sf) return sf; }
  /* simbolos C++ mangled (FMOD::...) vem das libs irmas libfmodex/libfmodevent */
  if(nm[0]=='_'&&nm[1]=='Z'){ extern void *dt_fmod_lookup(const char*); void *fp=dt_fmod_lookup(nm); if(fp) return fp; }
  void *p=dlsym(RTLD_DEFAULT,nm);
  if(p) return p;
  /* fmod C-API (FMOD_System_Create etc.) tambem pode vir das libs irmas */
  { extern void *dt_fmod_lookup(const char*); void *fp=dt_fmod_lookup(nm); if(fp) return fp; }
  /* Android-specific / nao-achado -> stub */
  return (void*)&stub_generic;
}


DynLibFunction dynlib_functions[] = {
  {"AAssetManager_open", 0},
  {"AAsset_close", 0},
  {"AAsset_read", 0},
  {"AConfiguration_delete", 0},
  {"AConfiguration_fromAssetManager", 0},
  {"AConfiguration_getCountry", 0},
  {"AConfiguration_getLanguage", 0},
  {"AConfiguration_new", 0},
  {"AInputEvent_getSource", 0},
  {"AInputEvent_getType", 0},
  {"AInputQueue_attachLooper", 0},
  {"AInputQueue_detachLooper", 0},
  {"AInputQueue_finishEvent", 0},
  {"AInputQueue_getEvent", 0},
  {"AInputQueue_preDispatchEvent", 0},
  {"AKeyEvent_getAction", 0},
  {"AKeyEvent_getKeyCode", 0},
  {"ALooper_addFd", 0},
  {"ALooper_pollAll", 0},
  {"ALooper_prepare", 0},
  {"AMotionEvent_getAction", 0},
  {"AMotionEvent_getAxisValue", 0},
  {"AMotionEvent_getPointerCount", 0},
  {"AMotionEvent_getPointerId", 0},
  {"AMotionEvent_getPressure", 0},
  {"AMotionEvent_getSize", 0},
  {"AMotionEvent_getX", 0},
  {"AMotionEvent_getY", 0},
  {"ANativeWindow_setBuffersGeometry", 0},
  {"FMOD_Debug_SetLevel", 0},
  {"FMOD_EventSystem_Create", 0},
  {"FMOD_Memory_Initialize", 0},
  {"_ZN4FMOD11EventSystem11getCategoryEPKcPPNS_13EventCategoryE", 0},
  {"_ZN4FMOD11EventSystem12getNumEventsEPi", 0},
  {"_ZN4FMOD11EventSystem14getMusicSystemEPPNS_11MusicSystemE", 0},
  {"_ZN4FMOD11EventSystem15getSystemObjectEPPNS_6SystemE", 0},
  {"_ZN4FMOD11EventSystem17getProjectByIndexEiPPNS_12EventProjectE", 0},
  {"_ZN4FMOD11EventSystem17set3DNumListenersEi", 0},
  {"_ZN4FMOD11EventSystem18getCategoryByIndexEiPPNS_13EventCategoryE", 0},
  {"_ZN4FMOD11EventSystem18getEventBySystemIDEjjPPNS_5EventE", 0},
  {"_ZN4FMOD11EventSystem19setReverbPropertiesEPK22FMOD_REVERB_PROPERTIES", 0},
  {"_ZN4FMOD11EventSystem20getEventByGUIDStringEPKcjPPNS_5EventE", 0},
  {"_ZN4FMOD11EventSystem22getReverbPresetByIndexEiP22FMOD_REVERB_PROPERTIESPPc", 0},
  {"_ZN4FMOD11EventSystem23set3DListenerAttributesEiPK11FMOD_VECTORS3_S3_S3_", 0},
  {"_ZN4FMOD11EventSystem26setReverbAmbientPropertiesEP22FMOD_REVERB_PROPERTIES", 0},
  {"_ZN4FMOD11EventSystem4initEijPvj", 0},
  {"_ZN4FMOD11EventSystem4loadEPKcP19FMOD_EVENT_LOADINFOPPNS_12EventProjectE", 0},
  {"_ZN4FMOD11EventSystem6unloadEv", 0},
  {"_ZN4FMOD11EventSystem6updateEv", 0},
  {"_ZN4FMOD11EventSystem7releaseEv", 0},
  {"_ZN4FMOD12ChannelGroup10getChannelEiPPNS_7ChannelE", 0},
  {"_ZN4FMOD12ChannelGroup14getNumChannelsEPi", 0},
  {"_ZN4FMOD14EventParameter8setValueEf", 0},
  {"_ZN4FMOD5Event11getCategoryEPPNS_13EventCategoryE", 0},
  {"_ZN4FMOD5Event11setCallbackEPF11FMOD_RESULTP10FMOD_EVENT23FMOD_EVENT_CALLBACKTYPEPvS5_S5_ES5_", 0},
  {"_ZN4FMOD5Event11setUserDataEPv", 0},
  {"_ZN4FMOD5Event13getMemoryInfoEjjPjP25FMOD_MEMORY_USAGE_DETAILS", 0},
  {"_ZN4FMOD5Event14get3DOcclusionEPfS1_", 0},
  {"_ZN4FMOD5Event14set3DOcclusionEff", 0},
  {"_ZN4FMOD5Event15get3DAttributesEP11FMOD_VECTORS2_S2_", 0},
  {"_ZN4FMOD5Event15getChannelGroupEPPNS_12ChannelGroupE", 0},
  {"_ZN4FMOD5Event15set3DAttributesEPK11FMOD_VECTORS3_S3_", 0},
  {"_ZN4FMOD5Event16getNumPropertiesEPi", 0},
  {"_ZN4FMOD5Event18getPropertyByIndexEiPvb", 0},
  {"_ZN4FMOD5Event18setPropertyByIndexEiPvb", 0},
  {"_ZN4FMOD5Event19getParameterByIndexEiPPNS_14EventParameterE", 0},
  {"_ZN4FMOD5Event4stopEb", 0},
  {"_ZN4FMOD5Event5startEv", 0},
  {"_ZN4FMOD5Event7getInfoEPiPPcP15FMOD_EVENT_INFO", 0},
  {"_ZN4FMOD5Event7getMuteEPb", 0},
  {"_ZN4FMOD5Event7releaseEbb", 0},
  {"_ZN4FMOD5Event7setMuteEb", 0},
  {"_ZN4FMOD5Event8getPitchEPf21FMOD_EVENT_PITCHUNITS", 0},
  {"_ZN4FMOD5Event8getStateEPj", 0},
  {"_ZN4FMOD5Event8setPitchEf21FMOD_EVENT_PITCHUNITS", 0},
  {"_ZN4FMOD5Event9getPausedEPb", 0},
  {"_ZN4FMOD5Event9getVolumeEPf", 0},
  {"_ZN4FMOD5Event9setPausedEb", 0},
  {"_ZN4FMOD5Event9setVolumeEf", 0},
  {"_ZN4FMOD5Sound12getSyncPointEiPP14FMOD_SYNCPOINT", 0},
  {"_ZN4FMOD5Sound16getSyncPointInfoEP14FMOD_SYNCPOINTPciPjj", 0},
  {"_ZN4FMOD6System13getMemoryInfoEjjPjP25FMOD_MEMORY_USAGE_DETAILS", 0},
  {"_ZN4FMOD6System13setFileSystemEPF11FMOD_RESULTPKciPjPPvS6_EPFS1_S5_S5_EPFS1_S5_S5_jS4_S5_EPFS1_S5_jS5_EPFS1_P18FMOD_ASYNCREADINFOS5_ESA_i", 0},
  {"_ZN4FMOD6System16setDSPBufferSizeEji", 0},
  {"_ZN4FMOD6System19getAdvancedSettingsEP21FMOD_ADVANCEDSETTINGS", 0},
  {"_ZN4FMOD6System19setAdvancedSettingsEP21FMOD_ADVANCEDSETTINGS", 0},
  {"_ZN4FMOD7Channel11setPositionEjj", 0},
  {"__android_log_print", 0},
  {"__assert2", 0},
  {"__cxa_atexit", 0},
  {"__cxa_finalize", 0},
  {"__errno", 0},
  {"__gnu_Unwind_Find_exidx", 0},
  {"__google_potentially_blocking_region_begin", 0},
  {"__google_potentially_blocking_region_end", 0},
  {"__register_atfork", 0},
  {"__sF", 0},
  {"__stack_chk_fail", 0},
  {"__stack_chk_guard", 0},
  {"abort", 0},
  {"accept", 0},
  {"acos", 0},
  {"acosf", 0},
  {"asin", 0},
  {"asinf", 0},
  {"atan", 0},
  {"atan2", 0},
  {"atan2f", 0},
  {"atof", 0},
  {"atoi", 0},
  {"atol", 0},
  {"bind", 0},
  {"ceil", 0},
  {"ceilf", 0},
  {"clearerr", 0},
  {"clock", 0},
  {"clock_gettime", 0},
  {"close", 0},
  {"connect", 0},
  {"cos", 0},
  {"cosf", 0},
  {"cosh", 0},
  {"difftime", 0},
  {"eglChooseConfig", (uintptr_t)&egl_shim_ChooseConfig},
  {"eglCreateContext", (uintptr_t)&egl_shim_CreateContext},
  {"eglCreateWindowSurface", (uintptr_t)&egl_shim_CreateWindowSurface},
  {"eglDestroyContext", (uintptr_t)&egl_shim_DestroyContext},
  {"eglDestroySurface", (uintptr_t)&egl_shim_DestroySurface},
  {"eglGetConfigAttrib", (uintptr_t)&egl_shim_GetConfigAttrib},
  {"eglGetDisplay", (uintptr_t)&egl_shim_GetDisplay},
  {"eglGetError", (uintptr_t)&egl_shim_GetError},
  {"eglInitialize", (uintptr_t)&egl_shim_Initialize},
  {"eglMakeCurrent", (uintptr_t)&egl_shim_MakeCurrent},
  {"eglQuerySurface", (uintptr_t)&egl_shim_QuerySurface},
  {"eglSwapBuffers", (uintptr_t)&egl_shim_SwapBuffers},
  {"eglSwapInterval", (uintptr_t)&egl_shim_SwapInterval},
  {"eglTerminate", (uintptr_t)&egl_shim_Terminate},
  {"exit", 0},
  {"exp", 0},
  {"fclose", 0},
  {"fcntl", 0},
  {"feof", 0},
  {"ferror", 0},
  {"fflush", 0},
  {"fgets", 0},
  {"fileno", 0},
  {"floor", 0},
  {"floorf", 0},
  {"fmod", 0},
  {"fmodf", 0},
  {"fopen", 0},
  {"fprintf", 0},
  {"fputc", 0},
  {"fputs", 0},
  {"fread", 0},
  {"free", 0},
  {"freopen", 0},
  {"frexp", 0},
  {"fscanf", 0},
  {"fseek", 0},
  {"fstat", 0},
  {"ftell", 0},
  {"fwrite", 0},
  {"getc", 0},
  {"getenv", 0},
  {"gettimeofday", 0},
  {"glActiveTexture", 0},
  {"glAttachShader", 0},
  {"glBindAttribLocation", 0},
  {"glBindBuffer", 0},
  {"glBindFramebuffer", 0},
  {"glBindRenderbuffer", 0},
  {"glBindTexture", 0},
  {"glBlendEquation", 0},
  {"glBlendFunc", 0},
  {"glBlendFuncSeparate", 0},
  {"glBufferData", 0},
  {"glBufferSubData", 0},
  {"glCheckFramebufferStatus", 0},
  {"glClear", 0},
  {"glClearColor", 0},
  {"glClearDepthf", 0},
  {"glClearStencil", 0},
  {"glColorMask", 0},
  {"glCompileShader", 0},
  {"glCompressedTexImage2D", 0},
  {"glCreateProgram", 0},
  {"glCreateShader", 0},
  {"glCullFace", 0},
  {"glDeleteBuffers", 0},
  {"glDeleteFramebuffers", 0},
  {"glDeleteProgram", 0},
  {"glDeleteRenderbuffers", 0},
  {"glDeleteShader", 0},
  {"glDeleteTextures", 0},
  {"glDepthFunc", 0},
  {"glDepthMask", 0},
  {"glDepthRangef", 0},
  {"glDisable", 0},
  {"glDisableVertexAttribArray", 0},
  {"glDrawArrays", 0},
  {"glDrawElements", 0},
  {"glEnable", 0},
  {"glEnableVertexAttribArray", 0},
  {"glFinish", 0},
  {"glFlush", 0},
  {"glFramebufferRenderbuffer", 0},
  {"glFramebufferTexture2D", 0},
  {"glGenBuffers", 0},
  {"glGenFramebuffers", 0},
  {"glGenRenderbuffers", 0},
  {"glGenTextures", 0},
  {"glGenerateMipmap", 0},
  {"glGetError", 0},
  {"glGetFloatv", 0},
  {"glGetFramebufferAttachmentParameteriv", 0},
  {"glGetIntegerv", 0},
  {"glGetProgramBinaryOES", 0},
  {"glGetProgramInfoLog", 0},
  {"glGetProgramiv", 0},
  {"glGetRenderbufferParameteriv", 0},
  {"glGetShaderInfoLog", 0},
  {"glGetShaderiv", 0},
  {"glGetString", 0},
  {"glGetUniformLocation", 0},
  {"glIsFramebuffer", 0},
  {"glIsProgram", 0},
  {"glIsRenderbuffer", 0},
  {"glLinkProgram", 0},
  {"glMapBufferOES", 0},
  {"glProgramBinaryOES", 0},
  {"glReadPixels", 0},
  {"glReleaseShaderCompiler", 0},
  {"glRenderbufferStorage", 0},
  {"glScissor", 0},
  {"glShaderSource", 0},
  {"glStencilFunc", 0},
  {"glStencilMask", 0},
  {"glStencilOp", 0},
  {"glTexImage2D", 0},
  {"glTexParameteri", 0},
  {"glTexSubImage2D", 0},
  {"glUniform1f", 0},
  {"glUniform1fv", 0},
  {"glUniform1i", 0},
  {"glUniform1iv", 0},
  {"glUniform2f", 0},
  {"glUniform2fv", 0},
  {"glUniform3fv", 0},
  {"glUniform4fv", 0},
  {"glUniformMatrix4fv", 0},
  {"glUnmapBufferOES", 0},
  {"glUseProgram", 0},
  {"glVertexAttribPointer", 0},
  {"glViewport", 0},
  {"gmtime", 0},
  {"inet_addr", 0},
  {"isalnum", 0},
  {"isalpha", 0},
  {"iscntrl", 0},
  {"islower", 0},
  {"isprint", 0},
  {"ispunct", 0},
  {"isspace", 0},
  {"isupper", 0},
  {"iswspace", 0},
  {"isxdigit", 0},
  {"ldexp", 0},
  {"listen", 0},
  {"localtime", 0},
  {"log", 0},
  {"log10", 0},
  {"logf", 0},
  {"longjmp", 0},
  {"lrand48", 0},
  {"malloc", 0},
  {"memalign", 0},
  {"memchr", 0},
  {"memcmp", 0},
  {"memcpy", 0},
  {"memmove", 0},
  {"memset", 0},
  {"mktime", 0},
  {"modf", 0},
  {"nice", 0},
  {"pipe", 0},
  {"pow", 0},
  {"powf", 0},
  {"printf", 0},
  {"pthread_attr_destroy", 0},
  {"pthread_attr_init", 0},
  {"pthread_attr_setdetachstate", 0},
  {"pthread_attr_setschedparam", 0},
  {"pthread_attr_setstacksize", 0},
  {"pthread_cond_broadcast", 0},
  {"pthread_cond_destroy", 0},
  {"pthread_cond_init", 0},
  {"pthread_cond_signal", 0},
  {"pthread_cond_timedwait", 0},
  {"pthread_cond_wait", 0},
  {"pthread_create", 0},
  {"pthread_exit", 0},
  {"pthread_getspecific", 0},
  {"pthread_join", 0},
  {"pthread_key_create", 0},
  {"pthread_key_delete", 0},
  {"pthread_mutex_destroy", 0},
  {"pthread_mutex_init", 0},
  {"pthread_mutex_lock", 0},
  {"pthread_mutex_trylock", 0},
  {"pthread_mutex_unlock", 0},
  {"pthread_mutexattr_init", 0},
  {"pthread_mutexattr_settype", 0},
  {"pthread_self", 0},
  {"pthread_setspecific", 0},
  {"putchar", 0},
  {"puts", 0},
  {"qsort", 0},
  {"raise", 0},
  {"rand", 0},
  {"read", 0},
  {"realloc", 0},
  {"recv", 0},
  {"recvfrom", 0},
  {"remove", 0},
  {"rename", 0},
  {"rewind", 0},
  {"sched_yield", 0},
  {"select", 0},
  {"sem_destroy", 0},
  {"sem_getvalue", 0},
  {"sem_init", 0},
  {"sem_post", 0},
  {"sem_trywait", 0},
  {"sem_wait", 0},
  {"send", 0},
  {"sendto", 0},
  {"setjmp", 0},
  {"setlocale", 0},
  {"setsockopt", 0},
  {"setvbuf", 0},
  {"shutdown", 0},
  {"sin", 0},
  {"sinf", 0},
  {"sinh", 0},
  {"sleep", 0},
  {"snprintf", 0},
  {"socket", 0},
  {"sprintf", 0},
  {"sqrt", 0},
  {"sqrtf", 0},
  {"srand", 0},
  {"srand48", 0},
  {"sscanf", 0},
  {"stat", 0},
  {"stderr", 0},
  {"stdin", 0},
  {"stdout", 0},
  {"strcasecmp", 0},
  {"strcat", 0},
  {"strchr", 0},
  {"strcmp", 0},
  {"strcoll", 0},
  {"strcpy", 0},
  {"strcspn", 0},
  {"strdup", 0},
  {"strerror", 0},
  {"strftime", 0},
  {"strlcpy", 0},
  {"strlen", 0},
  {"strncasecmp", 0},
  {"strncat", 0},
  {"strncmp", 0},
  {"strncpy", 0},
  {"strpbrk", 0},
  {"strrchr", 0},
  {"strstr", 0},
  {"strtod", 0},
  {"strtok", 0},
  {"strtol", 0},
  {"strtoul", 0},
  {"strtoull", 0},
  {"syscall", 0},
  {"system", 0},
  {"tan", 0},
  {"tanf", 0},
  {"tanh", 0},
  {"time", 0},
  {"tmpfile", 0},
  {"tmpnam", 0},
  {"tolower", 0},
  {"toupper", 0},
  {"ungetc", 0},
  {"usleep", 0},
  {"vsnprintf", 0},
  {"vsprintf", 0},
  {"wcscmp", 0},
  {"wcscoll", 0},
  {"wcscpy", 0},
  {"wcslen", 0},
  {"write", 0},
};
size_t dynlib_numfunctions = sizeof(dynlib_functions)/sizeof(dynlib_functions[0]);

void re4_fill(void){
  int real=0,stub=0;
  for(size_t i=0;i<dynlib_numfunctions;i++){
    if(dynlib_functions[i].func) continue;
    extern void *re4_resolve(const char*);
    void *p=re4_resolve(dynlib_functions[i].symbol);
    dynlib_functions[i].func=(uintptr_t)p; if(p)real++; else stub++;
  }
  fprintf(stderr,"[dt_fill] %d resolvidos %d nulos (de %zu)\n",real,stub,dynlib_numfunctions);
}
