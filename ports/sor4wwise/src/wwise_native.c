// wwise_native.c — wrapper glibc libWwise.so (PLUGIN do .NET) que so-carrega a
// libWwise REAL do APK (motor Wwise nativo) e faz trampolim dos native_wwise_*.
// Construtor: so_load + relocate + resolve(tabela combinada) + init_array.
// Resolve os imports que faltavam (libc/libm glibc, _chk bionic_shims, __sF, AAsset, liblog).
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <math.h>
#include <syslog.h>
#include <sched.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/auxv.h>
#include <link.h>
#include <pthread.h>
#include "so_util.h"
extern void opensles_shim_pump_callbacks(void);   // opensles_shim.c: dispara o BufferQueue cb da Wwise
// audioout.c — saida de audio CUSTOM (hibrido): SFX via manifest, musica via wem streamed
extern void ao_init(void);
extern void ao_post_event(const char* name);
extern void ao_music_request(const char* path);
extern void ao_music_close(const char* path);

extern DynLibFunction dynlib_functions[];
extern size_t dynlib_numfunctions;
extern void jni_shim_init(void**, void**);   // jni_shim.c: JavaVM/JNIEnv fake
static void* g_fake_vm=NULL; static void* g_fake_env=NULL;

// bionic_shims.c (funcoes reais)
extern void  *__memcpy_chk(void*,const void*,size_t,size_t);
extern void  *__memmove_chk(void*,const void*,size_t,size_t);
extern size_t __strlen_chk(const char*,size_t);
extern char  *__strncat_chk(char*,const char*,size_t,size_t);
extern char  *__strncpy_chk(char*,const char*,size_t,size_t);
extern char  *__strncpy_chk2(char*,const char*,size_t,size_t,size_t);
extern int    __vsnprintf_chk(char*,size_t,int,size_t,const char*,va_list);
extern size_t __system_property_get(const char*, char*);
extern void   android_set_abort_message(const char*);
extern char __sF[];   // bionic_shims.c: char __sF[3*512]

static void wlog(const char* s){
  const char* lp=getenv("WWISE_LOG"); if(!lp||!*lp) lp="/storage/roms/sor4-test/wwise.log";
  FILE* f=fopen(lp,"a"); if(f){fprintf(f,"%s\n",s); fclose(f);}
}

// ---------------- __sF (bionic stdio std streams) ----------------
// Wwise faz &__sF[idx]; nossos wrappers detectam o range e mandam p/ glibc.
#define SF_STRIDE 512
static FILE* sf_map(void* p){
  if((char*)p >= __sF && (char*)p < __sF+3*SF_STRIDE){
    long idx=((char*)p-__sF)/SF_STRIDE;
    return idx==0?stdin: idx==1?stdout: stderr;
  }
  return (FILE*)p;
}
static int w_fprintf(void* st,const char* fmt,...){ va_list a; va_start(a,fmt); int r=vfprintf(sf_map(st),fmt,a); va_end(a); return r; }
static int w_vfprintf(void* st,const char* fmt,va_list a){ return vfprintf(sf_map(st),fmt,a); }
static int w_fputc(int c,void* st){ return fputc(c,sf_map(st)); }
static int w_fputs(const char* s,void* st){ return fputs(s,sf_map(st)); }
static size_t w_fwrite(const void* p,size_t s,size_t n,void* st){ return fwrite(p,s,n,sf_map(st)); }
static int w_fflush(void* st){ return fflush(st?sf_map(st):NULL); }
static int w_feof(void* st){ return feof(sf_map(st)); }
static int w_fileno(void* st){ return fileno(sf_map(st)); }

// ---------------- liblog ----------------
static void w_openlog(const char* a,int b,int c){(void)a;(void)b;(void)c;}
static void w_closelog(void){}
// Wwise loga seus erros/monitor via syslog (build importa openlog/syslog/closelog).
// Roteamos p/ o wwise.log p/ ENXERGAR diagnosticos internos (codec/decode/init).
static void w_syslog(int pri,const char* fmt,...){
  (void)pri; char msg[1024]; va_list a; va_start(a,fmt); vsnprintf(msg,sizeof(msg),fmt,a); va_end(a);
  char b[1100]; snprintf(b,sizeof(b),"[WWISE-SYSLOG] %s",msg); wlog(b);
}

// ---------------- AAssetManager (carrega os bancos de g_bankbase) ----------------
static char g_bankbase[512]="/storage/roms/sor4-test/gameassets";
typedef struct { FILE* f; long len; int is_music; char path[1024]; } AAsset;
static void* w_AAssetManager_fromJava(void* env,void* obj){ (void)env;(void)obj; wlog("[wwise] AAssetManager_fromJava"); return (void*)0x1; }
static void* w_AAssetManager_open(void* mgr,const char* fn,int mode){
  (void)mgr;(void)mode;
  char path[1024];
  if(fn && fn[0]=='/') snprintf(path,sizeof(path),"%s",fn);           // ja absoluto (base+bank)
  else snprintf(path,sizeof(path),"%s%s",g_bankbase,fn?fn:"");        // relativo -> prefixa base
  FILE* f=fopen(path,"rb"); if(!f){ char b[1100]; snprintf(b,sizeof(b),"[wwise] AAsset open FALHOU %s",path); wlog(b); return NULL; }
  fseek(f,0,SEEK_END); long len=ftell(f); fseek(f,0,SEEK_SET);
  AAsset* a=malloc(sizeof(AAsset)); a->f=f; a->len=len; a->is_music=0; a->path[0]=0;
  char b[1100]; snprintf(b,sizeof(b),"[wwise] AAsset open %s (%ld)",path,len); wlog(b);
  // HIBRIDO: a Wwise nativa decide qual MUSICA tocar e abre o .wem streamed certo
  // (menu/fase/chefe). Echoamos esse wem p/ a saida de audio custom (OpenAL, em loop).
  // No CLOSE paramos no momento certo -> segue o fluxo original (para/troca de musica).
  { size_t pl=strlen(path); if(pl>4 && strcmp(path+pl-4,".wem")==0){ a->is_music=1; strncpy(a->path,path,sizeof(a->path)-1); ao_music_request(path); } }
  return a;
}
static unsigned long g_rd_calls=0, g_rd_bytes=0;
static int w_AAsset_read(void* as,void* buf,size_t cnt){ AAsset* a=as; if(!a||!a->f) return -1; int r=(int)fread(buf,1,cnt,a->f);
  if(getenv("WWISE_RDLOG")){ g_rd_calls++; if(r>0)g_rd_bytes+=r; if(g_rd_calls<=3||g_rd_calls%200==0){ char b[160]; snprintf(b,sizeof(b),"[wwise] AAsset_read #%lu cnt=%zu got=%d totbytes=%lu",g_rd_calls,cnt,r,g_rd_bytes); wlog(b);} }
  return r; }
static long w_AAsset_seek(void* as,long off,int whence){ AAsset* a=as; if(!a||!a->f) return -1; fseek(a->f,off,whence); return ftell(a->f); }
static long w_AAsset_getLength(void* as){ AAsset* a=as; return a?a->len:0; }
static void w_AAsset_close(void* as){ AAsset* a=as; if(a){ if(a->is_music) ao_music_close(a->path); if(a->f)fclose(a->f); free(a);} }
static void* w_AAssetManager_openDir(void* m,const char* d){ (void)m;(void)d; return NULL; }
static void w_AAssetDir_close(void* d){ (void)d; }

// ---------------- tabela extra (45 simbolos que faltavam) ----------------
static DynLibFunction extra[] = {
  // libm glibc
  {"acosf",(uintptr_t)&acosf},{"asinf",(uintptr_t)&asinf},{"exp2f",(uintptr_t)&exp2f},
  {"log10f",(uintptr_t)&log10f},{"sincos",(uintptr_t)&sincos},{"sincosf",(uintptr_t)&sincosf},
  // libc glibc
  {"feof",(uintptr_t)&w_feof},{"flockfile",(uintptr_t)&flockfile},{"freopen",(uintptr_t)&freopen},
  {"fseeko",(uintptr_t)&fseeko},{"ftello",(uintptr_t)&ftello},{"funlockfile",(uintptr_t)&funlockfile},
  {"getauxval",(uintptr_t)&getauxval},{"syscall",(uintptr_t)&syscall},{"vasprintf",(uintptr_t)&vasprintf},
  {"vfprintf",(uintptr_t)&w_vfprintf},{"dlopen",(uintptr_t)&dlopen},{"dlsym",(uintptr_t)&dlsym},
  {"dlclose",(uintptr_t)&dlclose},{"dlerror",(uintptr_t)&dlerror},{"dl_iterate_phdr",(uintptr_t)&dl_iterate_phdr},
  {"sched_get_priority_max",(uintptr_t)&sched_get_priority_max},{"sched_get_priority_min",(uintptr_t)&sched_get_priority_min},
  // stdio wrappers (p/ __sF)
  {"fprintf",(uintptr_t)&w_fprintf},{"fputc",(uintptr_t)&w_fputc},{"fputs",(uintptr_t)&w_fputs},
  {"fwrite",(uintptr_t)&w_fwrite},{"fflush",(uintptr_t)&w_fflush},
  {"__sF",(uintptr_t)__sF},
  // liblog
  {"openlog",(uintptr_t)&w_openlog},{"closelog",(uintptr_t)&w_closelog},{"syslog",(uintptr_t)&w_syslog},
  // bionic _chk + props
  {"__memcpy_chk",(uintptr_t)&__memcpy_chk},{"__memmove_chk",(uintptr_t)&__memmove_chk},
  {"__strlen_chk",(uintptr_t)&__strlen_chk},{"__strncat_chk",(uintptr_t)&__strncat_chk},
  {"__strncpy_chk",(uintptr_t)&__strncpy_chk},{"__strncpy_chk2",(uintptr_t)&__strncpy_chk2},
  {"__vsnprintf_chk",(uintptr_t)&__vsnprintf_chk},
  {"__system_property_get",(uintptr_t)&__system_property_get},
  {"android_set_abort_message",(uintptr_t)&android_set_abort_message},
  // AAssetManager (bancos)
  {"AAssetManager_fromJava",(uintptr_t)&w_AAssetManager_fromJava},
  {"AAssetManager_open",(uintptr_t)&w_AAssetManager_open},
  {"AAsset_read",(uintptr_t)&w_AAsset_read},{"AAsset_seek",(uintptr_t)&w_AAsset_seek},
  {"AAsset_getLength",(uintptr_t)&w_AAsset_getLength},{"AAsset_close",(uintptr_t)&w_AAsset_close},
  {"AAssetManager_openDir",(uintptr_t)&w_AAssetManager_openDir},{"AAssetDir_close",(uintptr_t)&w_AAssetDir_close},
};

// ---------------- carga da Wwise real ----------------
#define WWISE_REAL "/storage/roms/sor4-test/host_pkg/libs/libWwise.real.so"
#define HEAP_MB 64
static int g_loaded=0;
static int g_init_ok=0;   // so encaminha aos trampolins se a init REAL deu certo (senao crasha)
typedef int  (*fn_init_t)(const char*);
typedef void (*fn_void_t)(void);

static void load_real(void){
  if(g_loaded) return;
  const char* path=getenv("WWISE_REAL"); if(!path||!*path) path=WWISE_REAL;
  wlog("[wwise-native] carregando libWwise REAL...");
  size_t heap_size=(size_t)HEAP_MB*1024*1024;
  void* heap=mmap(NULL,heap_size,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  if(heap==MAP_FAILED){ wlog("[wwise-native] mmap heap FALHOU"); return; }
  if(so_load(path,heap,heap_size)<0){ wlog("[wwise-native] so_load FALHOU"); return; }
  if(so_relocate()<0){ wlog("[wwise-native] so_relocate FALHOU"); return; }
  // tabela combinada: extra primeiro (override) + dynlib_functions
  int ntot=(int)dynlib_numfunctions + (int)(sizeof(extra)/sizeof(extra[0]));
  DynLibFunction* comb=malloc(ntot*sizeof(DynLibFunction));
  int n=0;
  for(unsigned i=0;i<sizeof(extra)/sizeof(extra[0]);i++) comb[n++]=extra[i];
  for(size_t i=0;i<dynlib_numfunctions;i++) comb[n++]=dynlib_functions[i];
  if(so_resolve(comb,n,1)<0){ wlog("[wwise-native] so_resolve FALHOU"); free(comb); return; }
  free(comb);
  // Os 3 checks de PATH (AddBasePath/2o-check/SetBasePath) FALHAM no ambiente Android-fake
  // (validam path asset-relativo) mas NAO sao necessarios: nosso AAssetManager_open le os
  // bancos por path absoluto. NOPamos os b.ne deles p/ a init suceder. (offsets da v1.4.5)
  { const char* nz=getenv("WWISE_NOP");
    char defnop[]="0x12242c,0x122438,0x122510";
    if(!nz||!*nz) nz=defnop;
    if(nz&&*nz){ so_make_text_writable();
      char buf[256]; strncpy(buf,nz,sizeof(buf)-1); buf[sizeof(buf)-1]=0;
      char* p=buf; while(*p){ unsigned long off=strtoul(p,NULL,0);
        if(off){ *(uint32_t*)((char*)text_base+off)=0xd503201fu; char b[80]; snprintf(b,sizeof(b),"[wwise-native] NOP @0x%lx",off); wlog(b); }
        char* c=strchr(p,','); if(!c)break; p=c+1; }
      so_flush_caches();
    } }
  so_flush_caches();
  so_execute_init_array();
  g_loaded=1;
  { char b[160]; uintptr_t ni=so_find_addr("native_wwise_init");
    snprintf(b,sizeof(b),"[wwise-native] text_base=%p native_wwise_init=0x%lx (off 0x122350)",text_base,(unsigned long)ni); wlog(b); }
  wlog("[wwise-native] libWwise REAL carregada OK");
  // Wwise precisa de uma JavaVM (capturada via JNI_OnLoad pelo runtime Android, que nao temos).
  // Damos a JavaVM/JNIEnv FAKE do jni_shim e chamamos JNI_OnLoad + native_android_preinit nos.
  jni_shim_init(&g_fake_vm,&g_fake_env);
  uintptr_t jol=so_find_addr("JNI_OnLoad");
  if(jol){ int v=((int(*)(void*,void*))jol)(g_fake_vm,NULL); char b[64]; snprintf(b,sizeof(b),"[wwise-native] JNI_OnLoad=%d",v); wlog(b); }
  uintptr_t pre=so_find_addr("native_android_preinit");
  if(pre){ ((long(*)(void*))pre)((void*)0x1000); wlog("[wwise-native] native_android_preinit(fake activity) chamado"); }
  { const char* gp=getenv("WWISE_GDB"); if(gp&&*gp){ int s=atoi(gp); if(s<=0)s=40; so_make_text_writable(); char b[120]; snprintf(b,sizeof(b),"[wwise-native] PAUSANDO %ds p/ gdb (pid=%d text_base=%p)...",s,(int)getpid(),text_base); wlog(b); sleep(s); wlog("[wwise-native] resumindo"); } }
}

__attribute__((constructor)) static void ctor(void){ load_real(); }

#define CALL0(name,ret) ret name(void){ if(!g_loaded) load_real(); uintptr_t a=so_find_addr(#name); if(!a) return (ret)0; return ((ret(*)(void))a)(); }
#define TRAMP(name) uintptr_t __tr_##name(void){ if(!g_loaded) load_real(); return so_find_addr(#name); }

// ----- trampolins dos 21 native_wwise_* (+ preinit) -----
long native_android_preinit(void* activity){ if(!g_loaded) load_real(); char b[80]; snprintf(b,sizeof(b),"[wwise-native] preinit activity=%p",activity); wlog(b); uintptr_t a=so_find_addr("native_android_preinit"); return a?((long(*)(void*))a)(activity):0; }

static void force_volumes_now(void);   // fwd: forca RTPC MusicVolume/SfxVolume
static pthread_mutex_t g_render_mtx = PTHREAD_MUTEX_INITIALIZER;
static void render_audio_locked(void){
  uintptr_t a=so_find_addr("native_wwise_update"); if(!a) return;
  pthread_mutex_lock(&g_render_mtx);
  ((void(*)(void))a)();                 // = AK::SoundEngine::RenderAudio(true)
  pthread_mutex_unlock(&g_render_mtx);
}
// AUDIO THREAD UNICA acoplada: RenderAudio() (behavioral, prepara o frame) -> pump
// (dispara o callback do BufferQueue, onde a Wwise RENDERIZA o frame com params frescos).
// Acoplar elimina o desync entre behavioral e render que deixava o mix VAZIO.
static void* pump_thread_fn(void* a){ (void)a;
  { char b[64]; snprintf(b,sizeof(b),"[wwise-native] audio thread tid=%ld",syscall(SYS_gettid)); wlog(b);}
  // A engine nativa serve so p/ SELECIONAR a musica (abre o .wem certo por contexto);
  // o audio dela e' descartado. Tickamos devagar (~33Hz) p/ economizar CPU e nao
  // roubar tempo da thread de streaming de musica (OpenAL) -> sem stutter.
  unsigned us=20000; { const char* e=getenv("WWISE_TICK_US"); if(e&&*e) us=(unsigned)atoi(e); }
  for(;;){ render_audio_locked(); opensles_shim_pump_callbacks(); usleep(us); }
  return NULL;
}
static void start_pump_thread(void){
  static int started=0; if(started) return; started=1;
  pthread_t th; if(pthread_create(&th,NULL,pump_thread_fn,NULL)==0){ pthread_detach(th); wlog("[wwise-native] audio thread iniciada (RenderAudio+pump acoplados)"); }
  else wlog("[wwise-native] audio thread FALHOU");
}

int native_wwise_init(const char* p){
  if(!g_loaded) load_real();
  // O jogo passa get_data_folder() VAZIO no nosso port -> AddBasePath falha.
  // Substituimos pelo dir dos bancos (gameassets, com barra final).
  const char* base=getenv("SOR4_BANKDIR"); if(!base||!*base) base="/storage/roms/sor4-test/gameassets/";
  if(p&&*p) base=p;
  strncpy(g_bankbase,base,sizeof(g_bankbase)-1);
  char b[600]; snprintf(b,sizeof(b),"[wwise-native] init path='%s'",base); wlog(b);
  uintptr_t a=so_find_addr("native_wwise_init"); if(!a){ wlog("[wwise-native] init addr=0"); return 0; }
  int r=((int(*)(const char*))a)(base);
  g_init_ok = (r!=0);
  if(g_init_ok){ start_pump_thread(); force_volumes_now(); ao_init(); wlog("[wwise-native] volumes iniciais forcados pos-init + audioout custom"); }
  // diagnostico: le os globais que o 2o check (0x256b24) testa
  { unsigned char* fb=(unsigned char*)((char*)text_base + 0x2d0628);
    void** pl=(void**)((char*)text_base + 0x2d1dc0);
    char b3[160]; snprintf(b3,sizeof(b3),"[wwise-native] DIAG flag@2d0628=%d plist@2d1dc0=%p",(int)*fb,*pl); wlog(b3); }
  char b2[64]; snprintf(b2,sizeof(b2),"[wwise-native] real init=%d",r); wlog(b2);
  return r;
}
#define FWD_V(name) void name(void){ if(!g_init_ok) return; uintptr_t a=so_find_addr(#name); if(a)((void(*)(void))a)(); }
#define FWD_I0(name) int name(void){ if(!g_init_ok) return 0; uintptr_t a=so_find_addr(#name); return a?((int(*)(void))a)():0; }
#define FWD_S(name) int name(const char* s){ if(!g_init_ok) return 0; uintptr_t a=so_find_addr(#name); return a?((int(*)(const char*))a)(s):0; }

FWD_V(native_wwise_destroy)
FWD_S(native_wwise_loadbank)
FWD_S(native_wwise_unloadbank)
int native_wwise_post_event(const char* e){ if(!g_init_ok)return 0; ao_post_event(e); uintptr_t a=so_find_addr("native_wwise_post_event"); return a?((int(*)(const char*))a)(e):0; }
FWD_S(native_wwise_post_trigger)
int native_wwise_post_event_with_id(const char* e,uint64_t o){ if(!g_init_ok)return 0; ao_post_event(e); uintptr_t a=so_find_addr("native_wwise_post_event_with_id"); return a?((int(*)(const char*,uint64_t))a)(e,o):0; }
int native_wwise_post_trigger_with_id(const char* t,uint64_t o){ if(!g_init_ok)return 0; uintptr_t a=so_find_addr("native_wwise_post_trigger_with_id"); return a?((int(*)(const char*,uint64_t))a)(t,o):0; }
void native_wwise_register_gameobject_with_id(uint64_t id){ if(!g_init_ok)return; uintptr_t a=so_find_addr("native_wwise_register_gameobject_with_id"); if(a)((void(*)(uint64_t))a)(id); }
void native_wwise_register_gameobject_with_id_and_name(uint64_t id,const char* n){ if(!g_init_ok)return; uintptr_t a=so_find_addr("native_wwise_register_gameobject_with_id_and_name"); if(a)((void(*)(uint64_t,const char*))a)(id,n); }
void native_wwise_set_switch(const char* g,const char* s,uint64_t o){ if(!g_init_ok)return; { char b[200]; snprintf(b,sizeof(b),"[wwise-native] set_switch grp='%s' sw='%s' obj=%llu",g?g:"",s?s:"",(unsigned long long)o); wlog(b);} uintptr_t a=so_find_addr("native_wwise_set_switch"); if(a)((void(*)(const char*,const char*,uint64_t))a)(g,s,o); }
void native_wwise_set_state(const char* g,const char* s){ if(!g_init_ok)return; { char b[200]; snprintf(b,sizeof(b),"[wwise-native] set_state grp='%s' state='%s'",g?g:"",s?s:""); wlog(b);} uintptr_t a=so_find_addr("native_wwise_set_state"); if(a)((void(*)(const char*,const char*))a)(g,s); }
int native_wwise_set_listener_position(void* v){ if(!g_init_ok)return 0; uintptr_t a=so_find_addr("native_wwise_set_listener_position"); return a?((int(*)(void*))a)(v):0; }
int native_wwise_set_gameobject_position(uint64_t o,void* v){ if(!g_init_ok)return 0; uintptr_t a=so_find_addr("native_wwise_set_gameobject_position"); return a?((int(*)(uint64_t,void*))a)(o,v):0; }
// ---- volume forcado (RTPC MusicVolume/SfxVolume) ----
// O jogo zera esses RTPCs em Game_Deactivated (perda de foco); nossa janela SDL
// fake nunca reporta foco -> RTPC=0 -> Wwise renderiza SILENCIO (RAWpeak=0).
// Forcamos um piso audivel. WWISE_FORCEVOL=0 desliga; WWISE_VOLFLOOR=valor (default 1.0).
static int real_set_rtpc(const char* n,float v){ uintptr_t a=so_find_addr("native_wwise_set_rtpc_value"); return a?((int(*)(const char*,float))a)(n,v):0; }
static int forcevol(void){ const char* e=getenv("WWISE_FORCEVOL"); return (!e||!*e)?1:atoi(e); }
static float volfloor(void){ const char* e=getenv("WWISE_VOLFLOOR"); return (e&&*e)?(float)atof(e):1.0f; }
static int is_vol_rtpc(const char* n){ return n&&(strcmp(n,"MusicVolume")==0||strcmp(n,"SfxVolume")==0||strcmp(n,"Ambiance")==0); }
static void force_volumes_now(void){ if(!forcevol())return; float fl=volfloor(); real_set_rtpc("MusicVolume",fl); real_set_rtpc("SfxVolume",fl); }

void native_wwise_update(void){
  if(!g_init_ok) return;
  static unsigned uc=0;
  force_volumes_now();                       // sobrepoe qualquer zeragem por foco, todo frame
  render_audio_locked();                     // serializado com o render driver
  if(uc==0||uc==600||uc==3000){ char b[80]; snprintf(b,sizeof(b),"[wwise-native] update() chamada #%u (RenderAudio do JOGO)",uc); wlog(b);} uc++;
}

int native_wwise_set_rtpc_value(const char* n,float v){
  if(!g_init_ok)return 0;
  float orig=v; int forced=0;
  if(forcevol() && is_vol_rtpc(n)){ float fl=volfloor(); if(v<fl){ v=fl; forced=1; } }
  if(forced){ char b[160]; snprintf(b,sizeof(b),"[wwise-native] set_rtpc '%s' val=%.3f -> FORCADO p/ %.3f",n?n:"(null)",orig,v); wlog(b); }
  return real_set_rtpc(n,v);
}
int native_wwise_set_rtpc_value_with_id(const char* n,float v,uint64_t o){ if(!g_init_ok)return 0; uintptr_t a=so_find_addr("native_wwise_set_rtpc_value_with_id"); return a?((int(*)(const char*,float,uint64_t))a)(n,v,o):0; }
int native_wwise_get_music_event(void* ev){ if(!g_init_ok)return 0; uintptr_t a=so_find_addr("native_wwise_get_music_event"); return a?((int(*)(void*))a)(ev):0; }
int native_wwise_get_total_memory(void){ if(!g_init_ok)return 0; uintptr_t a=so_find_addr("native_wwise_get_total_memory"); return a?((int(*)(void))a)():0; }
int native_wwise_is_game_object_active(uint64_t o){ if(!g_init_ok)return 0; uintptr_t a=so_find_addr("native_wwise_is_game_object_active"); return a?((int(*)(uint64_t))a)(o):0; }
void native_wwise_unregister_inactive_game_objects(void){ if(!g_init_ok)return; uintptr_t a=so_find_addr("native_wwise_unregister_inactive_game_objects"); if(a)((void(*)(void))a)(); }
long get_total_memory_stub(void){ return 0; }
long sor4_gl_noop(void){ return 0; }
