#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <SDL2/SDL.h>
#include <signal.h>
#include <ucontext.h>
#include <pthread.h>
#include <stdarg.h>
#include <dlfcn.h>
#include <pwd.h>
#include <math.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include "so_util.h"
#include "imports.h"
#include "android_shim.h"
#include "jni_shim.h"
#include "opensles_shim.h"
/* RAIZ do "invalid CIL": glibc fstatat64 preenche struct stat64 layout GLIBC, mas Unity
   (bionic) le st_size no offset BIONIC -> tamanho errado (32KB) -> le so 32KB do .dll.
   Fix: syscall CRU -> o kernel preenche o layout stat64 do kernel (== bionic). */
static const char *re4_fixpath(const char *p);  /* '\'->'/' (paths de save Windows) */
static int my_fstatat64(int dfd,const char*p,void*b,int fl){ return (int)syscall(__NR_fstatat64,dfd,re4_fixpath(p),b,fl); }
static int my_fstat64(int fd,void*b){ return (int)syscall(__NR_fstat64,fd,b); }
static int my_stat64(const char*p,void*b){ return (int)syscall(__NR_stat64,re4_fixpath(p),b); }
static int my_lstat64(const char*p,void*b){ return (int)syscall(__NR_lstat64,re4_fixpath(p),b); }
static int my_access(const char*p,int m){ return access(re4_fixpath(p),m); }
static int my_unlink(const char*p){ return unlink(re4_fixpath(p)); }
static int my_rename(const char*a,const char*b){ char tmp[1024]; const char*fa=re4_fixpath(a);
  /* re4_fixpath usa buffer thread-local unico -> copia o 1o antes do 2o */
  strncpy(tmp,fa,sizeof tmp-1); tmp[sizeof tmp-1]=0; return rename(tmp,re4_fixpath(b)); }
static int my_mkdir(const char*p,unsigned m){ return mkdir(re4_fixpath(p),m); }
static uintptr_t g_mono_base=0, g_unity_base=0;
/* resolve um endereco p/ "libunity+0xOFF" / "libmono+0xOFF" / "0xADDR" (buffer thread-local).
   Usado pelo diagnostico de semaforo (pthread_shim) p/ achar a FUNCAO (offset estavel c/ ASLR). */
/* 1 se o endereco esta dentro da libunity (p/ o shim mirar os semaforos de job do engine). */
int re4_in_unity(uintptr_t a){ return g_unity_base && a>=g_unity_base && a<g_unity_base+0x2000000; }
const char *re4_addr_mod(uintptr_t a){
  static __thread char b[48];
  if(g_unity_base&&a>=g_unity_base&&a<g_unity_base+0x2000000) snprintf(b,sizeof b,"libunity+0x%lx",(unsigned long)(a-g_unity_base));
  else if(g_mono_base&&a>=g_mono_base&&a<g_mono_base+0x600000) snprintf(b,sizeof b,"libmono+0x%lx",(unsigned long)(a-g_mono_base));
  else snprintf(b,sizeof b,"0x%lx",(unsigned long)a);
  return b;
}
static void getmem_trace(const char*tag);
static const char *re4_gamedir(void);
void egl_shim_force_present(const char *reason);
static void (*g_orig_mono_add_internal_call)(const char*, const void*) = 0;
static char *(*g_mono_string_to_utf8_fn)(void*) = 0;
static void (*g_mono_free_fn)(void*) = 0;
static long my_read(int fd,void*b,unsigned long n){ long r=read(fd,b,n);
  if(n==32768||n>50000){ static int rn=0; if(rn++<24){ void*ra=__builtin_return_address(0); uintptr_t a=(uintptr_t)ra; const char*m="?"; uintptr_t o=a;
    if(g_mono_base&&a>=g_mono_base&&a<g_mono_base+0x600000){m="libmono";o=a-g_mono_base;} else if(g_unity_base&&a>=g_unity_base&&a<g_unity_base+0x2000000){m="libunity";o=a-g_unity_base;}
    fprintf(stderr,"[READ] fd=%d req=%lu -> %ld caller=%s+0x%lx\n",fd,n,r,m,(unsigned long)o); } } return r; }
static long my_write(int fd,const void*b,unsigned long n){ if(fd==2&&b&&n>0&&n<200){ static int wn=0; if(wn++<200){ char t[208]; unsigned k=n<200?n:199; memcpy(t,b,k); t[k]=0; for(unsigned i=0;i<k;i++) if(t[i]=='\n')t[i]=' '; fprintf(stderr,"[W] %s\n",t); } } return write(fd,b,n); }
/* BRIDGE stdio bionic: __sF[3] (stdin/out/err) do bionic tem layout != glibc. Forneco um
   array marcador + intercepto fprintf/fputs/etc pra mapear &__sF[i] -> stream real do glibc.
   Assim o LOG de erro da Unity (que vai pro stderr bionic) aparece. */
static char g_sf[3*84+16];
static FILE *sf_map(FILE *fp){ uintptr_t p=(uintptr_t)fp,b=(uintptr_t)g_sf;
  if(p>=b && p<b+sizeof(g_sf)){ int i=(int)((p-b)/84); return i<=0?stdin:(i==1?stdout:stderr); } return fp; }
static int my_fprintf(FILE*fp,const char*fmt,...){ if(fmt&&strstr(fmt,"GET_MEM"))getmem_trace("fprintf"); va_list ap; va_start(ap,fmt); int r=vfprintf(sf_map(fp),fmt,ap); va_end(ap); return r; }
static int my_vfprintf(FILE*fp,const char*fmt,va_list ap){ if(fmt&&strstr(fmt,"GET_MEM"))getmem_trace("vfprintf"); return vfprintf(sf_map(fp),fmt,ap); }
static void getmem_trace(const char*tag){ (void)tag; /* DESABILITADO: o scan estourava a pilha */ }
static int my_fputs(const char*str,FILE*fp){ if(str&&strstr(str,"GET_MEM"))getmem_trace("fputs"); return fputs(str,sf_map(fp)); }
static size_t my_fwrite(const void*p,size_t a,size_t b,FILE*fp){ if(p&&a*b>=7&&memmem(p,a*b,"GET_MEM",7))getmem_trace("fwrite"); return fwrite(p,a,b,sf_map(fp)); }
static int my_fputc(int c,FILE*fp){ return fputc(c,sf_map(fp)); }
static int my_fflush(FILE*fp){ return fflush(fp?sf_map(fp):NULL); }
/* loga dlopen/dlsym -> ve se a engine tenta carregar libmono (Mono runtime C#) */
static so_module *g_m_mono=NULL; static so_module *g_m_unity=NULL;
static char g_dl_self; /* sentinela do handle global/self */
/* AUDIO (FMOD do Unity): faz dlopen("libOpenSLES.so")+dlsym -> roteamos p/ o opensles_shim
   (OpenSL ES -> SDL2). Sem isso "FMOD failed to initialize the output device". (padrao do sonicmania) */
static char g_dl_sl; /* sentinela do handle de libOpenSLES */
SLresult slCreateEngine_shim(void **e,SLuint32 no,const void*eo,SLuint32 ni,const SLInterfaceID*ii,const SLBoolean*ir);
void opensles_shim_pump_callbacks(void);
extern const SLInterfaceID sl_IID_ENGINE, sl_IID_PLAY, sl_IID_VOLUME, sl_IID_BUFFERQUEUE,
  sl_IID_EFFECTSEND, sl_IID_ENGINECAPABILITIES, sl_IID_ENVIRONMENTALREVERB;
static void *my_dlopen(const char *nm,int flag){
  if(nm && strstr(nm,"OpenSLES")){ fprintf(stderr,"[DLOPEN] \"%s\" -> opensles_shim\n",nm); return &g_dl_sl; }
  if(!nm||!nm[0]||strstr(nm,"libc")||strstr(nm,"libunity")||strstr(nm,"libmain")||strstr(nm,"libmono")){
    fprintf(stderr,"[DLOPEN] \"%s\" -> SELF\n",nm?nm:"(null)"); return &g_dl_self; }
  void *h=dlopen(nm,flag); fprintf(stderr,"[DLOPEN] \"%s\" -> %p\n",nm,h); return h?h:&g_dl_self; }
static int noop_ret0(void){ return 0; }
static int re4_skip_fullscreen_movie_enabled(void){
  static int cached = -1;
  if(cached < 0){
    const char *env = getenv("RE4_SKIPMOVIE");
    cached = (!env || !env[0] || strcmp(env, "0") != 0) ? 1 : 0;
  }
  return cached;
}
static int my_icall_PlayFullScreenMovie(void *mono_string, const void *bg_color, int control_mode, int scaling_mode){
  (void)bg_color;
  char *utf8 = NULL;
  if(g_mono_string_to_utf8_fn && mono_string)
    utf8 = g_mono_string_to_utf8_fn(mono_string);
  fprintf(stderr,
          "[MOVIE] skip PlayFullScreenMovie path=%s control=%d scaling=%d\n",
          utf8 ? utf8 : "(null)", control_mode, scaling_mode);
  if(utf8 && g_mono_free_fn)
    g_mono_free_fn(utf8);
  return 1;
}
typedef float (*re4_input_axis_icall_t)(void*);
typedef int (*re4_input_button_icall_t)(void*);
typedef int (*re4_input_key_icall_t)(int);
typedef int (*re4_input_key_string_icall_t)(void*);
typedef int (*re4_input_anykey_icall_t)(void);
static re4_input_axis_icall_t g_orig_input_get_axis = 0;
static re4_input_axis_icall_t g_orig_input_get_axis_raw = 0;
static re4_input_button_icall_t g_orig_input_get_button = 0;
static re4_input_button_icall_t g_orig_input_get_button_down = 0;
static re4_input_button_icall_t g_orig_input_get_button_up = 0;
static re4_input_key_icall_t g_orig_input_get_key = 0;
static re4_input_key_icall_t g_orig_input_get_key_down = 0;
static re4_input_key_icall_t g_orig_input_get_key_up = 0;
static re4_input_key_string_icall_t g_orig_input_get_key_string = 0;
static re4_input_key_string_icall_t g_orig_input_get_key_down_string = 0;
static re4_input_key_string_icall_t g_orig_input_get_key_up_string = 0;
static re4_input_anykey_icall_t g_orig_input_any_key = 0;
static re4_input_anykey_icall_t g_orig_input_any_key_down = 0;
/* originais dos icalls de touch (cair no jogo quando NAO injetamos movimento -> nao trava no load) */
static int  (*g_orig_touchcount)(void) = 0;
static void (*g_orig_gettouch)(int,void*) = 0;
static float my_input_get_axis(void *mono_string);
static float my_input_get_axis_raw(void *mono_string);
static int my_input_get_button(void *mono_string);
static int my_input_get_button_down(void *mono_string);
static int my_input_get_button_up(void *mono_string);
static int my_input_get_key(int keycode);
static int my_input_get_key_down(int keycode);
static int my_input_get_key_up(int keycode);
static int my_input_get_key_string(void *mono_string);
static int my_input_get_key_down_string(void *mono_string);
static int my_input_get_key_up_string(void *mono_string);
static int my_input_any_key(void);
static int my_input_any_key_down(void);
/* MOUSE virtual: o menu CODEX e uGUI/EventSystem (touch/mouse). Hookamos a API de
   mouse do Unity para simular um cursor+clique que o StandaloneInputModule processa,
   navegando os botoes. Driver ao vivo via /tmp/re4mouse. */
static int my_input_get_mouse_button(int button);
static int my_input_get_mouse_button_down(int button);
static int my_input_get_mouse_button_up(int button);
static void my_icall_get_mousePosition(void *out_vec3);
static int my_input_get_mouse_present(void);
/* TOUCH virtual: o menu CODEX le Input.touchCount + Input.GetTouch (custom controller).
   Hookamos para reportar 1 toque na posicao do cursor com fase Began/Stationary/Ended. */
static int my_input_get_touch_count(void);
static void my_icall_get_touch(int index, void *out_touch);
static unsigned my_xinput_get_state(unsigned player_index, void *raw_state);
static void my_xinput_set_state(unsigned player_index, float left_motor, float right_motor);
static int re4_int_env(const char *name, int fallback, int min_value, int max_value);
static int re4_screen_height(void);
static int re4_input_hook_enabled(void){
  const char *v = getenv("RE4_NO_INPUTHOOK");
  return (!v || !v[0] || strcmp(v, "0") == 0);
}
static void my_mono_add_internal_call(const char *name, const void *method){
  const void *resolved = method;
  if(name){
    int is_input = strstr(name, "UnityEngine.Input::") != NULL;
    if((is_input || strstr(name, "Handheld") || strstr(name, "Movie") || strstr(name, "Video")) && method)
      fprintf(stderr, "[ICALL] %s -> %p\n", name, method);
    if(re4_input_hook_enabled() && is_input){
      if(strstr(name, "GetAxisRaw")){
        if(!g_orig_input_get_axis_raw) g_orig_input_get_axis_raw = (re4_input_axis_icall_t)method;
        resolved = (const void*)my_input_get_axis_raw;
        fprintf(stderr, "[ICALL] override %s -> SDL axis raw\n", name);
      } else if(strstr(name, "GetAxis")){
        if(!g_orig_input_get_axis) g_orig_input_get_axis = (re4_input_axis_icall_t)method;
        resolved = (const void*)my_input_get_axis;
        fprintf(stderr, "[ICALL] override %s -> SDL axis\n", name);
      } else if(strstr(name, "GetButtonDown")){
        if(!g_orig_input_get_button_down) g_orig_input_get_button_down = (re4_input_button_icall_t)method;
        resolved = (const void*)my_input_get_button_down;
        fprintf(stderr, "[ICALL] override %s -> SDL button down\n", name);
      } else if(strstr(name, "GetButtonUp")){
        if(!g_orig_input_get_button_up) g_orig_input_get_button_up = (re4_input_button_icall_t)method;
        resolved = (const void*)my_input_get_button_up;
        fprintf(stderr, "[ICALL] override %s -> SDL button up\n", name);
      } else if(strstr(name, "GetButton")){
        if(!g_orig_input_get_button) g_orig_input_get_button = (re4_input_button_icall_t)method;
        resolved = (const void*)my_input_get_button;
        fprintf(stderr, "[ICALL] override %s -> SDL button\n", name);
      } else if(strstr(name, "GetKeyDownString")){
        if(!g_orig_input_get_key_down_string) g_orig_input_get_key_down_string = (re4_input_key_string_icall_t)method;
        resolved = (const void*)my_input_get_key_down_string;
        fprintf(stderr, "[ICALL] override %s -> SDL key string down\n", name);
      } else if(strstr(name, "GetKeyDownInt")){
        if(!g_orig_input_get_key_down) g_orig_input_get_key_down = (re4_input_key_icall_t)method;
        resolved = (const void*)my_input_get_key_down;
        fprintf(stderr, "[ICALL] override %s -> SDL key down\n", name);
      } else if(strstr(name, "GetKeyUpString")){
        if(!g_orig_input_get_key_up_string) g_orig_input_get_key_up_string = (re4_input_key_string_icall_t)method;
        resolved = (const void*)my_input_get_key_up_string;
        fprintf(stderr, "[ICALL] override %s -> SDL key string up\n", name);
      } else if(strstr(name, "GetKeyUpInt")){
        if(!g_orig_input_get_key_up) g_orig_input_get_key_up = (re4_input_key_icall_t)method;
        resolved = (const void*)my_input_get_key_up;
        fprintf(stderr, "[ICALL] override %s -> SDL key up\n", name);
      } else if(strstr(name, "GetKeyString")){
        if(!g_orig_input_get_key_string) g_orig_input_get_key_string = (re4_input_key_string_icall_t)method;
        resolved = (const void*)my_input_get_key_string;
        fprintf(stderr, "[ICALL] override %s -> SDL key string\n", name);
      } else if(strstr(name, "GetKeyInt")){
        if(!g_orig_input_get_key) g_orig_input_get_key = (re4_input_key_icall_t)method;
        resolved = (const void*)my_input_get_key;
        fprintf(stderr, "[ICALL] override %s -> SDL key\n", name);
      } else if(strstr(name, "get_anyKeyDown")){
        if(!g_orig_input_any_key_down) g_orig_input_any_key_down = (re4_input_anykey_icall_t)method;
        resolved = (const void*)my_input_any_key_down;
        fprintf(stderr, "[ICALL] override %s -> SDL anyKeyDown\n", name);
      } else if(strstr(name, "get_anyKey")){
        if(!g_orig_input_any_key) g_orig_input_any_key = (re4_input_anykey_icall_t)method;
        resolved = (const void*)my_input_any_key;
        fprintf(stderr, "[ICALL] override %s -> SDL anyKey\n", name);
      } else if(getenv("RE4_MOUSEHOOK") && strstr(name, "GetMouseButtonDown")){
        resolved = (const void*)my_input_get_mouse_button_down;
        fprintf(stderr, "[ICALL] override %s -> virtual mouse down\n", name);
      } else if(getenv("RE4_MOUSEHOOK") && strstr(name, "GetMouseButtonUp")){
        resolved = (const void*)my_input_get_mouse_button_up;
        fprintf(stderr, "[ICALL] override %s -> virtual mouse up\n", name);
      } else if(getenv("RE4_MOUSEHOOK") && strstr(name, "GetMouseButton")){
        resolved = (const void*)my_input_get_mouse_button;
        fprintf(stderr, "[ICALL] override %s -> virtual mouse\n", name);
      } else if(getenv("RE4_MOUSEHOOK") && strstr(name, "INTERNAL_get_mousePosition")){
        resolved = (const void*)my_icall_get_mousePosition;
        fprintf(stderr, "[ICALL] override %s -> virtual mouse pos\n", name);
      } else if(getenv("RE4_MOUSEHOOK") && strstr(name, "get_mousePresent")){
        resolved = (const void*)my_input_get_mouse_present;
        fprintf(stderr, "[ICALL] override %s -> mousePresent=1\n", name);
      } else if(getenv("RE4_TOUCHHOOK") && strstr(name, "INTERNAL_CALL_GetTouch")){
        if(!g_orig_gettouch) g_orig_gettouch=(void(*)(int,void*))method;
        resolved = (const void*)my_icall_get_touch;
        fprintf(stderr, "[ICALL] override %s -> virtual touch\n", name);
      } else if(getenv("RE4_TOUCHHOOK") && strstr(name, "get_touchCount")){
        if(!g_orig_touchcount) g_orig_touchcount=(int(*)(void))method;
        resolved = (const void*)my_input_get_touch_count;
        fprintf(stderr, "[ICALL] override %s -> virtual touchCount\n", name);
      }
    }
    if(re4_skip_fullscreen_movie_enabled() &&
       (strstr(name, "PlayFullScreenMovie") || strstr(name, "INTERNAL_CALL_PlayFullScreenMovie"))){
      resolved = (const void*)my_icall_PlayFullScreenMovie;
      fprintf(stderr, "[ICALL] override %s -> %p\n", name, resolved);
    }
  }
  if(g_orig_mono_add_internal_call)
    g_orig_mono_add_internal_call(name, resolved);
}
/* DIAG GL: versao/GLSL + se os shaders sao ES3 (#version 300 es) e compilam (Mali=ES2) */
static const unsigned char* (*r_glGetString)(unsigned);
static void (*r_glShaderSource)(unsigned,int,const char*const*,const int*);
static void (*r_glCompileShader)(unsigned);
static void (*r_glGetShaderiv)(unsigned,unsigned,int*);
static void (*r_glGetShaderInfoLog)(unsigned,int,int*,char*);
static void (*r_glTexImage2D)(unsigned,int,int,int,int,int,unsigned,unsigned,const void*);
static void (*r_glRenderbufferStorage)(unsigned,unsigned,int,int);
static void (*r_glBindFramebuffer)(unsigned,unsigned);
static unsigned (*r_glCheckFramebufferStatus)(unsigned);
static void (*r_glFramebufferTexture2D)(unsigned,unsigned,unsigned,unsigned,int);
static void (*r_glFramebufferRenderbuffer)(unsigned,unsigned,unsigned,unsigned);
static void (*r_glReadPixels)(int,int,int,int,unsigned,unsigned,void*);
static void (*r_glClear)(unsigned);
static void (*r_glClearColor)(float,float,float,float);
static void (*r_glDrawArrays)(unsigned,int,int);
static void (*r_glDrawElements)(unsigned,int,unsigned,const void*);
static void (*r_glUseProgram)(unsigned);
static void (*r_glActiveTexture)(unsigned);
static void (*r_glBindTexture)(unsigned,unsigned);
static void (*r_glViewport)(int,int,int,int);
static void (*r_glScissor)(int,int,int,int);
static void (*r_glEnable)(unsigned);
static void (*r_glDisable)(unsigned);
static void (*r_glBlendFunc)(unsigned,unsigned);
static void (*r_glBlendFuncSeparate)(unsigned,unsigned,unsigned,unsigned);
static void (*r_glColorMask)(unsigned char,unsigned char,unsigned char,unsigned char);
static unsigned (*r_glCreateShader)(unsigned);
static unsigned (*r_glCreateProgram)(void);
static void (*r_glAttachShader)(unsigned,unsigned);
static void (*r_glLinkProgram)(unsigned);
static void (*r_glGetProgramiv)(unsigned,unsigned,int*);
static void (*r_glGetProgramInfoLog)(unsigned,int,int*,char*);
static int (*r_glGetUniformLocation)(unsigned,const char*);
static int (*r_glGetAttribLocation)(unsigned,const char*);
static void (*r_glUniform1i)(int,int);
static void (*r_glEnableVertexAttribArray)(unsigned);
static void (*r_glDisableVertexAttribArray)(unsigned);
static void (*r_glVertexAttribPointer)(unsigned,int,unsigned,unsigned char,int,const void*);
static void (*r_glDeleteShader)(unsigned);
static void (*r_glGetIntegerv)(unsigned,int*);
static void (*r_glBindBuffer)(unsigned,unsigned);
static void (*r_glTexParameteri)(unsigned,unsigned,int);
static void (*r_glGenTextures)(int,unsigned*);
static void (*r_glCopyTexImage2D)(unsigned,int,unsigned,int,int,int,int,int);
static unsigned (*r_glGetError)(void);
static unsigned char (*r_glIsEnabled)(unsigned);
static void (*r_glGetBooleanv)(unsigned,unsigned char*);
static void (*r_glUniform2f)(int,float,float);
static void (*r_glTexSubImage2D)(unsigned,int,int,int,int,int,unsigned,unsigned,const void*);
static unsigned g_snap_tex=0;   /* snapshot do composite (capturado quando FBO0 e valido) */
static int g_snap_w=0,g_snap_h=0;
static unsigned g_gl_bound_fbo=0;
int g_re4_frame=-1;      /* global: pthread_shim le p/ gatear o CONDBREAK so no gameplay carregado */
static int g_in_menu=0;  /* 1 = menu CODEX visivel; 0 = gameplay (reabilita injecao android_shim p/ mover Leon) */
int g_gameplay=0; /* 1 = entrou no gameplay -> PARA toda poke-Mono (evita FREEZE no gameplay/pause) */
int g_gameplay_frame=0; /* frame em que entrou no gameplay (p/ esperar a cena carregar antes do touch-move) */
/* Chamado pelo jni_shim quando o jogo grava 'sceneToLoad' (carga de cena = entrou no gameplay).
   Sinal CONFIAVEL (independe do nome do botao) -> liga touch-move e desliga poke-Mono. */
void re4_signal_gameplay(int on){
  if(on){ if(!g_gameplay){ g_gameplay=1; g_gameplay_frame=g_re4_frame; fprintf(stderr,"[GP] sceneToLoad -> GAMEPLAY START f=%d\n",g_re4_frame); fsync(2);} }
  else { if(g_gameplay){ g_gameplay=0; fprintf(stderr,"[GP] volta ao MENU f=%d\n",g_re4_frame); fsync(2);} }
}
/* ESTUDO: registra vocabulario DISTINTO de input consultado pelo jogo (sem cap).
   Gated por RE4_INDUMP. Imprime [INDUMP] <chave> uma vez por chave nova. */
static int re4_indump(const char *key){
  if(!getenv("RE4_INDUMP") || !key) return 0;
  static char *seen[512]; static int nseen=0;
  for(int i=0;i<nseen;i++) if(!strcmp(seen[i],key)) return 0;
  if(nseen < (int)(sizeof(seen)/sizeof(seen[0]))){
    seen[nseen++]=strdup(key);
    fprintf(stderr,"[INDUMP] %s (f=%d)\n", key, g_re4_frame);
    fsync(2);
  }
  return 1;
}
/* ===== RE4 input hook: SDL_GameController -> UnityEngine.Input / XInput =====
   O nativeInjectEvent funciona para alguns botões, mas Unity 2018 Mono não lê direção por
   MotionEvent nesse port. Estes hooks alimentam a API que o C# realmente consulta:
   UnityEngine.Input.GetAxis/GetButton/GetKey e XInputDotNetPure. */
static SDL_GameController *g_re4_gp_ctrl = NULL;
static int g_re4_gp_index = -1;
static int g_re4_gp_poll_frame = -999999;
static unsigned char g_re4_gp_btn[24], g_re4_gp_prev[24];
static float g_re4_gp_lx, g_re4_gp_ly, g_re4_gp_rx, g_re4_gp_ry, g_re4_gp_lt, g_re4_gp_rt;
static unsigned long g_re4_gp_polls;

enum {
  RE4_BTN_A = 0, RE4_BTN_B, RE4_BTN_X, RE4_BTN_Y,
  RE4_BTN_LB, RE4_BTN_RB, RE4_BTN_BACK, RE4_BTN_START,
  RE4_BTN_L3, RE4_BTN_R3, RE4_BTN_DU, RE4_BTN_DD, RE4_BTN_DL, RE4_BTN_DR,
  RE4_BTN_LT, RE4_BTN_RT, RE4_BTN_COUNT
};

static float re4_gp_axis_norm(Sint16 v){
  return v < 0 ? (float)v / 32768.0f : (float)v / 32767.0f;
}
static float re4_gp_axis_dead(float v, float dz){
  float a = fabsf(v);
  if(a <= dz) return 0.0f;
  if(dz < 0.0f) dz = 0.0f;
  if(dz > 0.9f) dz = 0.9f;
  return (v < 0.0f ? -1.0f : 1.0f) * ((a - dz) / (1.0f - dz));
}
static float re4_gp_clamp_unit(float v){
  if(v < -1.0f) return -1.0f;
  if(v > 1.0f) return 1.0f;
  return v;
}
static int re4_gp_have_pad(void){
  return (g_re4_gp_ctrl && SDL_GameControllerGetAttached(g_re4_gp_ctrl)) ||
         getenv("RE4_GPAUTO") || getenv("RE4_GPVIRT");
}
static void re4_gp_open(void){
  if(g_re4_gp_ctrl && SDL_GameControllerGetAttached(g_re4_gp_ctrl)) return;
  if(g_re4_gp_ctrl){
    SDL_GameControllerClose(g_re4_gp_ctrl);
    g_re4_gp_ctrl = NULL;
    g_re4_gp_index = -1;
  }
  if((SDL_WasInit(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) & SDL_INIT_GAMECONTROLLER) == 0){
    if(SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0){
      static int logged_init = 0;
      if(!logged_init++){
        fprintf(stderr, "[RGP] SDL_InitSubSystem(GAMECONTROLLER) falhou: %s\n", SDL_GetError());
        fsync(2);
      }
      return;
    }
  }
  if(getenv("RE4_GC_MAP")){
    static int added = 0;
    if(!added){
      int r = SDL_GameControllerAddMapping(getenv("RE4_GC_MAP"));
      fprintf(stderr, "[RGP] RE4_GC_MAP AddMapping -> %d (%s)\n", r, r < 0 ? SDL_GetError() : "ok");
      fsync(2);
      added = 1;
    }
  }
  int n = SDL_NumJoysticks();
  static int logged_n = -9999;
  if(n != logged_n || getenv("RE4_GPLOG")){
    fprintf(stderr, "[RGP] SDL_NumJoysticks=%d\n", n);
    for(int i = 0; i < n; i++)
      fprintf(stderr, "[RGP]  js%d: \"%s\" isGameController=%d\n",
              i, SDL_JoystickNameForIndex(i), SDL_IsGameController(i));
    fsync(2);
    logged_n = n;
  }
  for(int i = 0; i < n; i++){
    if(!SDL_IsGameController(i)) continue;
    g_re4_gp_ctrl = SDL_GameControllerOpen(i);
    if(g_re4_gp_ctrl){
      g_re4_gp_index = i;
      fprintf(stderr, "[RGP] Xbox layout via SDL_GameController js%d: %s\n",
              i, SDL_GameControllerName(g_re4_gp_ctrl));
      fsync(2);
      return;
    }
    fprintf(stderr, "[RGP] SDL_GameControllerOpen(%d) falhou: %s\n", i, SDL_GetError());
    fsync(2);
  }
}
static void re4_gp_apply_virtual(void){
  if(getenv("RE4_GPAUTO") && g_re4_frame > 120){
    int per = re4_int_env("RE4_GPAUTO_PERIOD", 180, 30, 2000);
    int ph = g_re4_frame % per;
    if(ph < per / 2) g_re4_gp_ly = -1.0f;
  }
  if(getenv("RE4_GPVIRT")){
    static int vframes[RE4_BTN_COUNT];
    static int axframes; static float axx, axy, arx, ary;
    FILE *vf = fopen("/tmp/re4gp", "r");
    if(vf){
      char tok[48] = {0};
      int got = fscanf(vf, "%47s", tok) == 1 && tok[0];
      fclose(vf);
      if(got){
        vf = fopen("/tmp/re4gp", "w");
        if(vf) fclose(vf);
        int dur = re4_int_env("RE4_GPVDUR", 5, 1, 120);
        static const char *names[RE4_BTN_COUNT] = {
          "a","b","x","y","lb","rb","back","start","l3","r3",
          "up","down","left","right","lt","rt"
        };
        for(int i = 0; i < RE4_BTN_COUNT; i++)
          if(!strcasecmp(tok, names[i])){
            vframes[i] = dur;
            fprintf(stderr, "[RGPV] %s -> btn[%d] x%d\n", tok, i, dur);
            fsync(2);
          }
        if(!strncasecmp(tok, "ls:", 3)){
          int x = 0, y = 0;
          if(sscanf(tok + 3, "%d:%d", &x, &y) == 2){
            axx = (float)x / 100.0f; axy = (float)y / 100.0f; axframes = dur;
            fprintf(stderr, "[RGPV] ls -> %.2f,%.2f x%d\n", axx, axy, dur);
            fsync(2);
          }
        } else if(!strncasecmp(tok, "rs:", 3)){
          int x = 0, y = 0;
          if(sscanf(tok + 3, "%d:%d", &x, &y) == 2){
            arx = (float)x / 100.0f; ary = (float)y / 100.0f; axframes = dur;
            fprintf(stderr, "[RGPV] rs -> %.2f,%.2f x%d\n", arx, ary, dur);
            fsync(2);
          }
        }
      }
    }
    for(int i = 0; i < RE4_BTN_COUNT; i++){
      if(vframes[i] > 0){ g_re4_gp_btn[i] = 1; vframes[i]--; }
    }
    if(axframes > 0){
      g_re4_gp_lx = axx; g_re4_gp_ly = axy; g_re4_gp_rx = arx; g_re4_gp_ry = ary;
      axframes--;
    }
  }
}
static void re4_gp_apply_stick_dpad(void){
  if(getenv("RE4_NO_STICKDPAD") || getenv("RE4_NO_STICK_DPAD")) return;
  if(g_re4_gp_lx < -0.55f) g_re4_gp_btn[RE4_BTN_DL] = 1;
  if(g_re4_gp_lx >  0.55f) g_re4_gp_btn[RE4_BTN_DR] = 1;
  if(g_re4_gp_ly < -0.55f) g_re4_gp_btn[RE4_BTN_DU] = 1;
  if(g_re4_gp_ly >  0.55f) g_re4_gp_btn[RE4_BTN_DD] = 1;
}
/* raw_ps2: REMAPEAMENTO MANUAL de botoes por indice cru. DESLIGADO por padrao.
   CAUSA-RAIZ do "preso no BACK / botoes embaralhados" (2026-06-17): a sessao
   anterior chutou indices {2,1,3,0,6,7,...} que NAO batem com este adaptador.
   O mapeamento CERTO (gamecontrollerdb do device, "USB Gamepad" 0810:0001) e
   a:b1 b:b2 x:b0 y:b3 dpad=hat lb:b4 rb:b5 lt:b6 rt:b7 -> o SDL_GameController ja
   aplica isso quando SDL_GAMECONTROLLERCONFIG esta setado (o que o RE4.sh faz via
   get_controls). Os outros jogos funcionam justamente por confiar no SDL. Entao
   confiamos no SDL e so usamos raw_ps2 se o usuario forcar RE4_RAW_PS2=1. */
static int re4_gp_raw_ps2_enabled(void){
  const char *force = getenv("RE4_RAW_PS2");
  return force && atoi(force) != 0;
}
static void re4_gp_apply_raw_ps2_buttons(void){
  if(!g_re4_gp_ctrl || !re4_gp_raw_ps2_enabled()) return;
  SDL_Joystick *js = SDL_GameControllerGetJoystick(g_re4_gp_ctrl);
  if(!js) return;
  static int logged = 0;
  if(!logged++){
    fprintf(stderr, "[RGP] using raw PS2/Twin USB button map\n");
    fsync(2);
  }
  static const int raw_buttons[10] = {
    2, 1, 3, 0, 6, 7, 8, 9, 10, 11
  };
  int nb = SDL_JoystickNumButtons(js);
  for(int i = 0; i < 10; i++)
    g_re4_gp_btn[i] = (raw_buttons[i] < nb && SDL_JoystickGetButton(js, raw_buttons[i])) ? 1 : 0;
  int h = SDL_JoystickNumHats(js) > 0 ? SDL_JoystickGetHat(js, 0) : 0;
  g_re4_gp_btn[RE4_BTN_DU] = (h & SDL_HAT_UP) ? 1 : 0;
  g_re4_gp_btn[RE4_BTN_DD] = (h & SDL_HAT_DOWN) ? 1 : 0;
  g_re4_gp_btn[RE4_BTN_DL] = (h & SDL_HAT_LEFT) ? 1 : 0;
  g_re4_gp_btn[RE4_BTN_DR] = (h & SDL_HAT_RIGHT) ? 1 : 0;
  /* DRIFT-FIX: o analogico destes adaptadores PS2/USB baratos NAO e calibrado
     (rest fica fora de centro, ex. Vertical=0.397 parado) -> passa do deadzone ->
     vira movimento-FANTASMA continuo -> tempestade de GetButtonDown no menu ->
     cursor voa e cola no BACK. A direcao confiavel aqui e o D-PAD (hat) lido acima.
     Entao ZERAMOS o analogico no modo raw_ps2. RE4_PS2_ANALOG=1 reabilita (se o
     controle tiver analogico bom). Os sticks tambem alimentam apply_stick_dpad,
     entao zerar aqui impede o drift de criar dpad falso. */
  if(!getenv("RE4_PS2_ANALOG")){
    g_re4_gp_lx = g_re4_gp_ly = g_re4_gp_rx = g_re4_gp_ry = 0.0f;
  }
}
static void re4_gp_poll(void){
  if(g_re4_gp_poll_frame == g_re4_frame) return;
  g_re4_gp_poll_frame = g_re4_frame;
  g_re4_gp_polls++;
  memcpy(g_re4_gp_prev, g_re4_gp_btn, sizeof(g_re4_gp_prev));
  memset(g_re4_gp_btn, 0, sizeof(g_re4_gp_btn));
  g_re4_gp_lx = g_re4_gp_ly = g_re4_gp_rx = g_re4_gp_ry = 0.0f;
  g_re4_gp_lt = g_re4_gp_rt = 0.0f;

  if(!getenv("RE4_GP_VIRTONLY")) re4_gp_open();
  if(g_re4_gp_ctrl && !getenv("RE4_GP_VIRTONLY")){
    SDL_GameControllerUpdate();
    /* DIAG: loga eixos/botoes CRUS do joystick para descobrir o estado de REPOUSO
       real do adaptador (RE4_RAWAXLOG). Sem isso so vejo o valor ja processado. */
    if(getenv("RE4_RAWAXLOG") && (g_re4_frame<400 || (g_re4_frame%30)==0)){
      SDL_Joystick *rj = SDL_GameControllerGetJoystick(g_re4_gp_ctrl);
      if(rj){
        int na=SDL_JoystickNumAxes(rj), nh=SDL_JoystickNumHats(rj);
        char buf[256]; int o=0;
        o+=snprintf(buf+o,sizeof(buf)-o,"[RAWAX] f=%d axes(%d):",g_re4_frame,na);
        for(int i=0;i<na && o<(int)sizeof(buf)-16;i++)
          o+=snprintf(buf+o,sizeof(buf)-o," a%d=%d",i,SDL_JoystickGetAxis(rj,i));
        o+=snprintf(buf+o,sizeof(buf)-o," gcLY=%d gcLX=%d hat0=%d",
          SDL_GameControllerGetAxis(g_re4_gp_ctrl,SDL_CONTROLLER_AXIS_LEFTY),
          SDL_GameControllerGetAxis(g_re4_gp_ctrl,SDL_CONTROLLER_AXIS_LEFTX),
          nh>0?SDL_JoystickGetHat(rj,0):-1);
        fprintf(stderr,"%s\n",buf); fsync(2);
      }
    }
    /* DIAG GUIADO: loga QUALQUER mudanca de botao/hat/eixo cru (RE4_RAWALL).
       Permite descobrir exatamente o que o adaptador do usuário envia por input. */
    if(getenv("RE4_RAWALL")){
      SDL_Joystick *rj = SDL_GameControllerGetJoystick(g_re4_gp_ctrl);
      if(rj){
        static int pb[32], ph=-1; static int paxd[8]; static int init=0;
        int na=SDL_JoystickNumAxes(rj), nbq=SDL_JoystickNumButtons(rj), nh=SDL_JoystickNumHats(rj);
        if(na>8)na=8; if(nbq>32)nbq=32;
        int h = nh>0?SDL_JoystickGetHat(rj,0):0;
        if(!init){ init=1; ph=h; for(int i=0;i<nbq;i++) pb[i]=SDL_JoystickGetButton(rj,i);
          for(int i=0;i<na;i++) paxd[i]=SDL_JoystickGetAxis(rj,i)/4000; }
        for(int i=0;i<nbq;i++){ int v=SDL_JoystickGetButton(rj,i);
          if(v!=pb[i]){ fprintf(stderr,"[RAWALL] f=%d BUTTON %d -> %d\n",g_re4_frame,i,v); fsync(2); pb[i]=v; } }
        if(h!=ph){ fprintf(stderr,"[RAWALL] f=%d HAT0 -> %d (U%d D%d L%d R%d)\n",g_re4_frame,h,
          !!(h&SDL_HAT_UP),!!(h&SDL_HAT_DOWN),!!(h&SDL_HAT_LEFT),!!(h&SDL_HAT_RIGHT)); fsync(2); ph=h; }
        for(int i=0;i<na;i++){ int d=SDL_JoystickGetAxis(rj,i)/4000;
          if(d!=paxd[i]){ fprintf(stderr,"[RAWALL] f=%d AXIS %d -> %d (~%d)\n",g_re4_frame,i,SDL_JoystickGetAxis(rj,i),d); fsync(2); paxd[i]=d; } }
      }
    }
    float dz = getenv("RE4_GP_DEADZONE") ? atof(getenv("RE4_GP_DEADZONE")) : 0.18f;
    g_re4_gp_lx = re4_gp_axis_dead(re4_gp_axis_norm(SDL_GameControllerGetAxis(g_re4_gp_ctrl, SDL_CONTROLLER_AXIS_LEFTX)), dz);
    g_re4_gp_ly = re4_gp_axis_dead(re4_gp_axis_norm(SDL_GameControllerGetAxis(g_re4_gp_ctrl, SDL_CONTROLLER_AXIS_LEFTY)), dz);
    g_re4_gp_rx = re4_gp_axis_dead(re4_gp_axis_norm(SDL_GameControllerGetAxis(g_re4_gp_ctrl, SDL_CONTROLLER_AXIS_RIGHTX)), dz);
    g_re4_gp_ry = re4_gp_axis_dead(re4_gp_axis_norm(SDL_GameControllerGetAxis(g_re4_gp_ctrl, SDL_CONTROLLER_AXIS_RIGHTY)), dz);
    g_re4_gp_lt = re4_gp_axis_norm(SDL_GameControllerGetAxis(g_re4_gp_ctrl, SDL_CONTROLLER_AXIS_TRIGGERLEFT));
    g_re4_gp_rt = re4_gp_axis_norm(SDL_GameControllerGetAxis(g_re4_gp_ctrl, SDL_CONTROLLER_AXIS_TRIGGERRIGHT));
    if(g_re4_gp_lt < 0.0f) g_re4_gp_lt = 0.0f;
    if(g_re4_gp_rt < 0.0f) g_re4_gp_rt = 0.0f;
    if(g_re4_gp_lt > 1.0f) g_re4_gp_lt = 1.0f;
    if(g_re4_gp_rt > 1.0f) g_re4_gp_rt = 1.0f;
#define RGBTN(idx, sdlbtn) g_re4_gp_btn[(idx)] = SDL_GameControllerGetButton(g_re4_gp_ctrl, (sdlbtn)) ? 1 : 0
    RGBTN(RE4_BTN_A, SDL_CONTROLLER_BUTTON_A);
    RGBTN(RE4_BTN_B, SDL_CONTROLLER_BUTTON_B);
    RGBTN(RE4_BTN_X, SDL_CONTROLLER_BUTTON_X);
    RGBTN(RE4_BTN_Y, SDL_CONTROLLER_BUTTON_Y);
    RGBTN(RE4_BTN_LB, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    RGBTN(RE4_BTN_RB, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    RGBTN(RE4_BTN_BACK, SDL_CONTROLLER_BUTTON_BACK);
    RGBTN(RE4_BTN_START, SDL_CONTROLLER_BUTTON_START);
    RGBTN(RE4_BTN_L3, SDL_CONTROLLER_BUTTON_LEFTSTICK);
    RGBTN(RE4_BTN_R3, SDL_CONTROLLER_BUTTON_RIGHTSTICK);
    RGBTN(RE4_BTN_DU, SDL_CONTROLLER_BUTTON_DPAD_UP);
    RGBTN(RE4_BTN_DD, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    RGBTN(RE4_BTN_DL, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    RGBTN(RE4_BTN_DR, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
#undef RGBTN
    re4_gp_apply_raw_ps2_buttons();
    g_re4_gp_btn[RE4_BTN_LT] = g_re4_gp_lt > 0.30f ? 1 : 0;
    g_re4_gp_btn[RE4_BTN_RT] = g_re4_gp_rt > 0.30f ? 1 : 0;
  }
  re4_gp_apply_virtual();
  re4_gp_apply_stick_dpad();
  if(getenv("RE4_GPAXLOG")){
    static int lf = -9999;
    if(g_re4_frame != lf && (g_re4_frame < 300 || (g_re4_frame % 30) == 0)){
      fprintf(stderr,
              "[RGPAX] f=%d pad=%d lx=%.2f ly=%.2f rx=%.2f ry=%.2f lt=%.2f rt=%.2f dpad=%d%d%d%d abxy=%d%d%d%d start=%d back=%d\n",
              g_re4_frame, g_re4_gp_index, g_re4_gp_lx, g_re4_gp_ly, g_re4_gp_rx, g_re4_gp_ry,
              g_re4_gp_lt, g_re4_gp_rt,
              g_re4_gp_btn[RE4_BTN_DU], g_re4_gp_btn[RE4_BTN_DD],
              g_re4_gp_btn[RE4_BTN_DL], g_re4_gp_btn[RE4_BTN_DR],
              g_re4_gp_btn[RE4_BTN_A], g_re4_gp_btn[RE4_BTN_B],
              g_re4_gp_btn[RE4_BTN_X], g_re4_gp_btn[RE4_BTN_Y],
              g_re4_gp_btn[RE4_BTN_START], g_re4_gp_btn[RE4_BTN_BACK]);
      fsync(2);
      lf = g_re4_frame;
    }
  }
}
static void re4_gp_apply_motion_event(const FakeInputEvent *ev){
  if(!ev) return;
  float dz = getenv("RE4_GP_DEADZONE") ? atof(getenv("RE4_GP_DEADZONE")) : 0.18f;
  g_re4_gp_lx = re4_gp_axis_dead(ev->axes[AMOTION_EVENT_AXIS_X], dz);
  g_re4_gp_ly = re4_gp_axis_dead(ev->axes[AMOTION_EVENT_AXIS_Y], dz);
  g_re4_gp_rx = re4_gp_axis_dead(ev->axes[AMOTION_EVENT_AXIS_Z], dz);
  g_re4_gp_ry = re4_gp_axis_dead(ev->axes[AMOTION_EVENT_AXIS_RZ], dz);
  g_re4_gp_lt = ev->axes[AMOTION_EVENT_AXIS_LTRIGGER];
  g_re4_gp_rt = ev->axes[AMOTION_EVENT_AXIS_RTRIGGER];
  if(g_re4_gp_lt < 0.0f) g_re4_gp_lt = 0.0f;
  if(g_re4_gp_rt < 0.0f) g_re4_gp_rt = 0.0f;
  if(g_re4_gp_lt > 1.0f) g_re4_gp_lt = 1.0f;
  if(g_re4_gp_rt > 1.0f) g_re4_gp_rt = 1.0f;
  g_re4_gp_btn[RE4_BTN_LT] = g_re4_gp_lt > 0.30f ? 1 : g_re4_gp_btn[RE4_BTN_LT];
  g_re4_gp_btn[RE4_BTN_RT] = g_re4_gp_rt > 0.30f ? 1 : g_re4_gp_btn[RE4_BTN_RT];
  float hx = ev->axes[AMOTION_EVENT_AXIS_HAT_X];
  float hy = ev->axes[AMOTION_EVENT_AXIS_HAT_Y];
  if(hx < -0.5f) g_re4_gp_btn[RE4_BTN_DL] = 1;
  if(hx >  0.5f) g_re4_gp_btn[RE4_BTN_DR] = 1;
  if(hy < -0.5f) g_re4_gp_btn[RE4_BTN_DU] = 1;
  if(hy >  0.5f) g_re4_gp_btn[RE4_BTN_DD] = 1;
  re4_gp_apply_stick_dpad();
}
static int re4_gp_button_id_from_name(const char *name){
  if(!name || !name[0]) return -1;
  if(!strcasecmp(name, "A") || !strcasecmp(name, "Submit") ||
     !strcasecmp(name, "Cross") ||
     !strcasecmp(name, "Fire1") || !strcasecmp(name, "Action") ||
     !strcasecmp(name, "Interact") || !strcasecmp(name, "Use") ||
     !strcasecmp(name, "Confirm") || !strcasecmp(name, "Jump"))
    return getenv("RE4_AB_SWAP") ? RE4_BTN_B : RE4_BTN_A;
  if(!strcasecmp(name, "B") || !strcasecmp(name, "Cancel") ||
     !strcasecmp(name, "Circle") || !strcasecmp(name, "Escape"))
    return getenv("RE4_AB_SWAP") ? RE4_BTN_A : RE4_BTN_B;
  if(!strcasecmp(name, "X") || !strcasecmp(name, "Fire3") ||
     !strcasecmp(name, "Square") || !strcasecmp(name, "Reload") ||
     !strcasecmp(name, "Run")) return RE4_BTN_X;
  if(!strcasecmp(name, "Y") || !strcasecmp(name, "Triangle") ||
     !strcasecmp(name, "Inventory") ||
     !strcasecmp(name, "Crouch")) return RE4_BTN_Y;
  if(!strcasecmp(name, "LB") || !strcasecmp(name, "L1") ||
     !strcasecmp(name, "Knife")) return RE4_BTN_LB;
  if(!strcasecmp(name, "RB") || !strcasecmp(name, "R1") ||
     !strcasecmp(name, "Aim") || !strcasecmp(name, "Fire")) return RE4_BTN_RB;
  if(!strcasecmp(name, "LT") || !strcasecmp(name, "L2") ||
     !strcasecmp(name, "Fire4")) return RE4_BTN_LT;
  if(!strcasecmp(name, "RT") || !strcasecmp(name, "R2") ||
     !strcasecmp(name, "Fire2") || !strcasecmp(name, "Shoot")) return RE4_BTN_RT;
  if(!strcasecmp(name, "Back") || !strcasecmp(name, "Select") ||
     !strcasecmp(name, "View")) return RE4_BTN_BACK;
  if(!strcasecmp(name, "Start") || !strcasecmp(name, "Pause") ||
     !strcasecmp(name, "Menu")) return RE4_BTN_START;
  if(!strcasecmp(name, "LeftStickClick") || !strcasecmp(name, "LeftThumb") || !strcasecmp(name, "L3")) return RE4_BTN_L3;
  if(!strcasecmp(name, "RightStickClick") || !strcasecmp(name, "RightThumb") || !strcasecmp(name, "R3")) return RE4_BTN_R3;
  if(!strcasecmp(name, "DPadUp") || !strcasecmp(name, "D-Pad Up") ||
     !strcasecmp(name, "D Pad Up") || !strcasecmp(name, "Up")) return RE4_BTN_DU;
  if(!strcasecmp(name, "DPadDown") || !strcasecmp(name, "D-Pad Down") ||
     !strcasecmp(name, "D Pad Down") || !strcasecmp(name, "Down")) return RE4_BTN_DD;
  if(!strcasecmp(name, "DPadLeft") || !strcasecmp(name, "D-Pad Left") ||
     !strcasecmp(name, "D Pad Left") || !strcasecmp(name, "Left")) return RE4_BTN_DL;
  if(!strcasecmp(name, "DPadRight") || !strcasecmp(name, "D-Pad Right") ||
     !strcasecmp(name, "D Pad Right") || !strcasecmp(name, "Right")) return RE4_BTN_DR;
  if(!strncasecmp(name, "joystick button ", 16) ||
     (!strncasecmp(name, "joystick ", 9) && strcasestr(name, " button "))){
    const char *p = strcasestr(name, "button ");
    int n = p ? atoi(p + 7) : atoi(name + 16);
    static const int map[] = {
      RE4_BTN_A, RE4_BTN_B, RE4_BTN_X, RE4_BTN_Y,
      RE4_BTN_LB, RE4_BTN_RB, RE4_BTN_BACK, RE4_BTN_START,
      RE4_BTN_L3, RE4_BTN_R3
    };
    if(n >= 0 && n < (int)(sizeof(map) / sizeof(map[0]))) return map[n];
  }
  return -1;
}
static float re4_gp_axis_from_name(const char *name, int *known){
  if(known) *known = 1;
  if(!name) { if(known) *known = 0; return 0.0f; }
  if(!strcasecmp(name, "Horizontal") || !strcasecmp(name, "LeftAnalogHorizontal") ||
     !strcasecmp(name, "Left Stick X") || !strcasecmp(name, "LeftStickX") ||
     !strcasecmp(name, "MoveHorizontal") || !strcasecmp(name, "HorizontalArrow"))
    return re4_gp_clamp_unit(g_re4_gp_lx + (g_re4_gp_btn[RE4_BTN_DR] ? 1.0f : 0.0f) - (g_re4_gp_btn[RE4_BTN_DL] ? 1.0f : 0.0f));
  if(!strcasecmp(name, "Vertical") || !strcasecmp(name, "LeftAnalogVertical") ||
     !strcasecmp(name, "Left Stick Y") || !strcasecmp(name, "LeftStickY") ||
     !strcasecmp(name, "MoveVertical") || !strcasecmp(name, "VerticalArrow"))
    return re4_gp_clamp_unit(-g_re4_gp_ly + (g_re4_gp_btn[RE4_BTN_DU] ? 1.0f : 0.0f) - (g_re4_gp_btn[RE4_BTN_DD] ? 1.0f : 0.0f));
  if(!strcasecmp(name, "RightAnalogHorizontal") || !strcasecmp(name, "Right Stick X") ||
     !strcasecmp(name, "RightStickX") || !strcasecmp(name, "CameraHorizontal") ||
     !strcasecmp(name, "LookHorizontal") || !strcasecmp(name, "Mouse X"))
    return g_re4_gp_rx;
  if(!strcasecmp(name, "RightAnalogVertical") || !strcasecmp(name, "Right Stick Y") ||
     !strcasecmp(name, "RightStickY") || !strcasecmp(name, "CameraVertical") ||
     !strcasecmp(name, "LookVertical") || !strcasecmp(name, "Mouse Y"))
    return -g_re4_gp_ry;
  if(!strcasecmp(name, "D-Pad Horizontal") || !strcasecmp(name, "D - Pad Horizontal") ||
     !strcasecmp(name, "DPadHorizontal") || !strcasecmp(name, "D Pad Horizontal"))
    return (g_re4_gp_btn[RE4_BTN_DR] ? 1.0f : 0.0f) - (g_re4_gp_btn[RE4_BTN_DL] ? 1.0f : 0.0f);
  if(!strcasecmp(name, "D-Pad Vertical") || !strcasecmp(name, "D - Pad Vertical") ||
     !strcasecmp(name, "DPadVertical") || !strcasecmp(name, "D Pad Vertical"))
    return (g_re4_gp_btn[RE4_BTN_DU] ? 1.0f : 0.0f) - (g_re4_gp_btn[RE4_BTN_DD] ? 1.0f : 0.0f);
  if(!strcasecmp(name, "LT") || !strcasecmp(name, "L2")) return g_re4_gp_lt;
  if(!strcasecmp(name, "RT") || !strcasecmp(name, "R2")) return g_re4_gp_rt;
  if(!strcasecmp(name, "Mouse ScrollWheel")) return 0.0f;
  if(known) *known = 0;
  return 0.0f;
}
static int re4_gp_axis_button_kind_from_name(const char *name){
  if(!name) return 0;
  if(!strcasecmp(name, "Horizontal") || !strcasecmp(name, "LeftAnalogHorizontal") ||
     !strcasecmp(name, "Left Stick X") || !strcasecmp(name, "LeftStickX") ||
     !strcasecmp(name, "MoveHorizontal") || !strcasecmp(name, "HorizontalArrow") ||
     !strcasecmp(name, "D-Pad Horizontal") || !strcasecmp(name, "DPadHorizontal"))
    return 1;
  if(!strcasecmp(name, "Vertical") || !strcasecmp(name, "LeftAnalogVertical") ||
     !strcasecmp(name, "Left Stick Y") || !strcasecmp(name, "LeftStickY") ||
     !strcasecmp(name, "MoveVertical") || !strcasecmp(name, "VerticalArrow") ||
     !strcasecmp(name, "D-Pad Vertical") || !strcasecmp(name, "DPadVertical"))
    return 2;
  return 0;
}
static int re4_gp_axis_button_sign(int kind){
  if(kind == 1){
    if(g_re4_gp_btn[RE4_BTN_DL] && !g_re4_gp_btn[RE4_BTN_DR]) return -1;
    if(g_re4_gp_btn[RE4_BTN_DR] && !g_re4_gp_btn[RE4_BTN_DL]) return 1;
    if(g_re4_gp_lx <= -0.55f) return -1;
    if(g_re4_gp_lx >= 0.55f) return 1;
  } else if(kind == 2){
    if(g_re4_gp_btn[RE4_BTN_DU] && !g_re4_gp_btn[RE4_BTN_DD]) return 1;
    if(g_re4_gp_btn[RE4_BTN_DD] && !g_re4_gp_btn[RE4_BTN_DU]) return -1;
    float v = -g_re4_gp_ly;
    if(v >= 0.55f) return 1;
    if(v <= -0.55f) return -1;
  }
  return 0;
}
static int re4_gp_axis_button_state(int kind, int edge){
  int now = 0, prev = 0;
  static int last_sign[3] = {0, 0, 0};
  static int hold_start[3] = {-1, -1, -1};
  static int last_down_frame[3] = {-999999, -999999, -999999};
  int sign = re4_gp_axis_button_sign(kind);
  int sign_changed = sign != last_sign[kind];
  if(sign_changed){
    hold_start[kind] = -1;
    last_sign[kind] = sign;
  }
  now = sign != 0;
  prev = (kind == 1) ?
    (g_re4_gp_prev[RE4_BTN_DL] || g_re4_gp_prev[RE4_BTN_DR]) :
    (g_re4_gp_prev[RE4_BTN_DU] || g_re4_gp_prev[RE4_BTN_DD]);
  if(edge == 0) return now;
  if(edge < 0){
    if(!now) hold_start[kind] = -1;
    return !now && prev;
  }
  if(!now){
    hold_start[kind] = -1;
    return 0;
  }
  if(!prev || hold_start[kind] < 0) hold_start[kind] = g_re4_frame;
  int fire = !prev || sign_changed;
  if(!fire){
    int delay = re4_int_env("RE4_MENU_REPEAT_DELAY", 18, 4, 120);
    int interval = re4_int_env("RE4_MENU_REPEAT_INTERVAL", 6, 2, 60);
    int held = g_re4_frame - hold_start[kind];
    fire = held >= delay && ((held - delay) % interval) == 0;
  }
  if(fire && last_down_frame[kind] != g_re4_frame){
    last_down_frame[kind] = g_re4_frame;
    return 1;
  }
  return 0;
}
static int re4_gp_submit_like_name(const char *name){
  return name && (!strcasecmp(name, "Submit") || !strcasecmp(name, "Confirm"));
}
static int re4_gp_cancel_like_name(const char *name){
  return name && (!strcasecmp(name, "Cancel") || !strcasecmp(name, "Escape"));
}
static int re4_input_button_orig(void *mono_string, int edge){
  if(edge == 0 && g_orig_input_get_button) return g_orig_input_get_button(mono_string);
  if(edge > 0 && g_orig_input_get_button_down) return g_orig_input_get_button_down(mono_string);
  if(edge < 0 && g_orig_input_get_button_up) return g_orig_input_get_button_up(mono_string);
  return 0;
}
/* MODO ESTRITO (default ON): para nomes de input que o MENU do RE4 usa
   (Submit/Cancel/Vertical/Horizontal), NAO cair no input original do Unity.
   O jni_shim enumera um joystick fake -> o backend do Unity gera valores-FANTASMA
   nesses eixos/botoes -> o cursor pula sozinho e fica preso em BACK/Cancel, e
   ciclando as opcoes. Estrito = devolve SO o estado do nosso gamepad SDL.
   Desliga com RE4_GP_NOSTRICT=1 (debug). */
static int re4_gp_strict(void){
  static int c=-1;
  /* DEFAULT OFF: descoberto 2026-06-17 que o estrito (retornar 0 quando nao temos
     input) BLOQUEIA o input REAL do Unity alimentado pelos eventos injetados
     (android_shim -> nativeInjectEvent -> estado de input do Unity -> GetAxis/
     GetButton ORIGINAL). Com o raw_ps2 desligado nao ha mais botao embaralhado,
     entao nao precisamos do estrito; deixamos o original fluir. RE4_GP_STRICT=1 liga. */
  if(c<0) c = getenv("RE4_GP_NOSTRICT") ? 0 : 1;  /* default ON: mata o fantasma que cicla a selecao */
  return c;
}
/* Fallback ao original SO quando NAO estrito (debug). */
static int re4_input_button_fallback(void *mono_string, int edge){
  if(re4_gp_strict()) return 0;
  return re4_input_button_orig(mono_string, edge);
}
static char *re4_mono_string_to_utf8(void *mono_string){
  if(!mono_string || !g_mono_string_to_utf8_fn) return NULL;
  return g_mono_string_to_utf8_fn(mono_string);
}
static void re4_mono_free_utf8(char *s){
  if(s && g_mono_free_fn) g_mono_free_fn(s);
}
static float my_input_get_axis_common(void *mono_string, int raw){
  char *name = re4_mono_string_to_utf8(mono_string);
  re4_gp_poll();
  if(getenv("RE4_INDUMP")){ char k[80]; snprintf(k,sizeof k,"%s(\"%s\")", raw?"GetAxisRaw":"GetAxis", name?name:"(null)"); re4_indump(k); }
  int known = 0;
  float v = re4_gp_axis_from_name(name, &known);
  if(getenv("RE4_AXMON") && known && name && (g_re4_frame%30)==0 &&
     (!strcasecmp(name,"Vertical")||!strcasecmp(name,"Horizontal"))){
    fprintf(stderr,"[AXMON] %s(\"%s\")=%.3f f=%d\n", raw?"GetAxisRaw":"GetAxis", name, v, g_re4_frame); fsync(2);
  }
  if(getenv("RE4_GPTRACE")){
    static int tn = 0;
    if(tn++ < 240){
      fprintf(stderr, "[RTRACE] %s(\"%s\") known=%d v=%.3f f=%d\n",
              raw ? "GetAxisRaw" : "GetAxis", name ? name : "(null)", known, v, g_re4_frame);
      fsync(2);
    }
  }
  if(known){
    if(getenv("RE4_GPLOG") && v != 0.0f){
      static int n = 0;
      if(n++ < 160){
        fprintf(stderr, "[RINPUT] %s(\"%s\") -> %.3f f=%d\n",
                raw ? "GetAxisRaw" : "GetAxis", name ? name : "(null)", v, g_re4_frame);
        fsync(2);
      }
    }
    re4_mono_free_utf8(name);
    return v;
  }
  re4_mono_free_utf8(name);
  if(raw && g_orig_input_get_axis_raw) return g_orig_input_get_axis_raw(mono_string);
  if(!raw && g_orig_input_get_axis) return g_orig_input_get_axis(mono_string);
  return 0.0f;
}
static float my_input_get_axis(void *mono_string){ return my_input_get_axis_common(mono_string, 0); }
static float my_input_get_axis_raw(void *mono_string){ return my_input_get_axis_common(mono_string, 1); }
static int my_input_get_button_common(void *mono_string, int edge){
  char *name = re4_mono_string_to_utf8(mono_string);
  re4_gp_poll();
  if(getenv("RE4_INDUMP")){ char k[80]; snprintf(k,sizeof k,"%s(\"%s\")", edge==0?"GetButton":(edge>0?"GetButtonDown":"GetButtonUp"), name?name:"(null)"); re4_indump(k); }
  /* CAMINHO UNICO (default): o nosso re4_menu_nav dirige o menu via Mono (move cursor,
     invoca onClick). Entao SUPRIMIMOS a entrada de menu do StandaloneInputModule do jogo
     (Submit/Cancel/Vertical/Horizontal) p/ NAO haver duplo-clique nem o auto-ciclo do
     modulo. Sao nomes so de menu (o gameplay usa GetKey/GetAxis). RE4_MENU_NOSOLE reativa. */
  if(g_in_menu && !getenv("RE4_MENU_NOSOLE") && name &&
     (!strcasecmp(name,"Submit")||!strcasecmp(name,"Cancel")||
      !strcasecmp(name,"Vertical")||!strcasecmp(name,"Horizontal"))){
    re4_mono_free_utf8(name);
    return 0;
  }
  int axis_kind = re4_gp_axis_button_kind_from_name(name);
  if(axis_kind){
    int r = re4_gp_axis_button_state(axis_kind, edge);
    if(getenv("RE4_GPTRACE")){
      static int tn = 0;
      if(tn++ < 240)
        fprintf(stderr, "[RTRACE] %s(\"%s\") axis_button=%d -> %d f=%d\n",
                edge == 0 ? "GetButton" : (edge > 0 ? "GetButtonDown" : "GetButtonUp"),
                name ? name : "(null)", axis_kind, r, g_re4_frame);
    }
    if(r){
      if(getenv("RE4_GPLOG")){
        fprintf(stderr, "[RINPUT] %s(\"%s\") -> %d f=%d\n",
                edge == 0 ? "GetButton" : (edge > 0 ? "GetButtonDown" : "GetButtonUp"),
                name ? name : "(null)", r, g_re4_frame);
        fsync(2);
      }
      re4_mono_free_utf8(name);
      return r;
    }
    int orig = re4_input_button_fallback(mono_string, edge);
    re4_mono_free_utf8(name);
    return orig;
  }
  if(re4_gp_submit_like_name(name)){
    int submit = getenv("RE4_AB_SWAP") ? RE4_BTN_B : RE4_BTN_A;
    int now = g_re4_gp_btn[submit] ? 1 : 0;
    int prev = g_re4_gp_prev[submit] ? 1 : 0;
    int r = edge == 0 ? now : (edge > 0 ? (now && !prev) : (!now && prev));
    if(r){
      if(getenv("RE4_GPLOG")) fprintf(stderr,"[RINPUT] %s(\"Submit\") -> %d f=%d\n",
        edge==0?"GetButton":(edge>0?"GetButtonDown":"GetButtonUp"), r, g_re4_frame);
      re4_mono_free_utf8(name);
      return r;
    }
    int orig = re4_input_button_fallback(mono_string, edge);
    re4_mono_free_utf8(name);
    return orig;
  }
  if(re4_gp_cancel_like_name(name)){
    int cancel = getenv("RE4_AB_SWAP") ? RE4_BTN_A : RE4_BTN_B;
    int now = g_re4_gp_btn[cancel] ? 1 : 0;
    int prev = g_re4_gp_prev[cancel] ? 1 : 0;
    int r = edge == 0 ? now : (edge > 0 ? (now && !prev) : (!now && prev));
    if(r){
      if(getenv("RE4_GPLOG")) fprintf(stderr,"[RINPUT] %s(\"Cancel\") -> %d f=%d\n",
        edge==0?"GetButton":(edge>0?"GetButtonDown":"GetButtonUp"), r, g_re4_frame);
      re4_mono_free_utf8(name);
      return r;
    }
    int orig = re4_input_button_fallback(mono_string, edge);
    re4_mono_free_utf8(name);
    return orig;
  }
  int id = re4_gp_button_id_from_name(name);
  if(getenv("RE4_GPTRACE")){
    static int tn = 0;
    if(tn++ < 240)
      fprintf(stderr, "[RTRACE] %s(\"%s\") id=%d f=%d\n",
              edge == 0 ? "GetButton" : (edge > 0 ? "GetButtonDown" : "GetButtonUp"),
              name ? name : "(null)", id, g_re4_frame);
  }
  if(id >= 0 && id < RE4_BTN_COUNT){
    int now = g_re4_gp_btn[id] ? 1 : 0;
    int prev = g_re4_gp_prev[id] ? 1 : 0;
    int r = edge == 0 ? now : (edge > 0 ? (now && !prev) : (!now && prev));
    if(getenv("RE4_GPLOG") && r){
      static int n = 0;
      if(n++ < 160){
        fprintf(stderr, "[RINPUT] %s(\"%s\") -> %d f=%d\n",
                edge == 0 ? "GetButton" : (edge > 0 ? "GetButtonDown" : "GetButtonUp"),
                name ? name : "(null)", r, g_re4_frame);
        fsync(2);
      }
    }
    re4_mono_free_utf8(name);
    if(r) return r;
    return re4_input_button_fallback(mono_string, edge);
  }
  re4_mono_free_utf8(name);
  /* nome DESCONHECIDO: ai sim deixa o original (pode ser algo que nao mapeamos). */
  return re4_input_button_orig(mono_string, edge);
}
static int my_input_get_button(void *mono_string){ return my_input_get_button_common(mono_string, 0); }
static int my_input_get_button_down(void *mono_string){ return my_input_get_button_common(mono_string, 1); }
static int my_input_get_button_up(void *mono_string){ return my_input_get_button_common(mono_string, -1); }
static int re4_gp_button_from_unity_key(int keycode){
  if(keycode >= 330 && keycode <= 369){
    static const int joy_map[20] = {
      RE4_BTN_A, RE4_BTN_B, RE4_BTN_X, RE4_BTN_Y,
      RE4_BTN_LB, RE4_BTN_RB, RE4_BTN_BACK, RE4_BTN_START,
      RE4_BTN_L3, RE4_BTN_R3,
      RE4_BTN_A, RE4_BTN_B, RE4_BTN_X, RE4_BTN_Y,
      RE4_BTN_LB, RE4_BTN_RB, RE4_BTN_BACK, RE4_BTN_START,
      RE4_BTN_L3, RE4_BTN_R3
    };
    return joy_map[(keycode - 330) % 20];
  }
  switch(keycode){
    case 13: return RE4_BTN_A;      /* Return = confirmar */
    case 27: return RE4_BTN_B;      /* Escape = cancelar */
    case 273: return RE4_BTN_DU;    /* UpArrow */
    case 274: return RE4_BTN_DD;    /* DownArrow */
    case 275: return RE4_BTN_DR;    /* RightArrow */
    case 276: return RE4_BTN_DL;    /* LeftArrow */
    /* TECLADO WASD+Space: o menu CODEX (MainMenu_KeyboardController) e o gameplay
       leem MOVIMENTO/ACAO por estas teclas (GetKey 119/115/97/100/32), NAO pelo
       EventSystem (Submit/Vertical). Mapear o gamepad aqui e o que faz o menu
       navegar/entrar e o Leon andar. RE4_NO_WASD desliga (debug). */
    case 119: return getenv("RE4_NO_WASD") ? -1 : RE4_BTN_DU; /* W = cima/frente */
    case 115: return getenv("RE4_NO_WASD") ? -1 : RE4_BTN_DD; /* S = baixo/tras */
    case 97:  return getenv("RE4_NO_WASD") ? -1 : RE4_BTN_DL; /* A = esquerda */
    case 100: return getenv("RE4_NO_WASD") ? -1 : RE4_BTN_DR; /* D = direita */
    case 32:  return getenv("RE4_NO_WASD") ? -1 : RE4_BTN_A;  /* Space = confirmar/acao */
    default: return -1;
  }
}
static int my_input_get_key_common(int keycode, int edge){
  re4_gp_poll();
  if(getenv("RE4_INDUMP")){ char k[64]; snprintf(k,sizeof k,"%s(%d)", edge==0?"GetKey":(edge>0?"GetKeyDown":"GetKeyUp"), keycode); re4_indump(k); }
  int id = re4_gp_button_from_unity_key(keycode);
  if(getenv("RE4_GPTRACE")){
    static int tn = 0;
    if(tn++ < 240)
      fprintf(stderr, "[RTRACE] %s(%d) id=%d f=%d\n",
              edge == 0 ? "GetKey" : (edge > 0 ? "GetKeyDown" : "GetKeyUp"),
              keycode, id, g_re4_frame);
  }
  if(id >= 0 && id < RE4_BTN_COUNT){
    int now = g_re4_gp_btn[id] ? 1 : 0;
    int prev = g_re4_gp_prev[id] ? 1 : 0;
    int r = edge == 0 ? now : (edge > 0 ? (now && !prev) : (!now && prev));
    if(getenv("RE4_GPLOG") && r){
      static int n = 0;
      if(n++ < 160){
        fprintf(stderr, "[RINPUT] %s(%d) -> %d f=%d\n",
                edge == 0 ? "GetKey" : (edge > 0 ? "GetKeyDown" : "GetKeyUp"),
                keycode, r, g_re4_frame);
        fsync(2);
      }
    }
    return r;
  }
  if(edge == 0 && g_orig_input_get_key) return g_orig_input_get_key(keycode);
  if(edge > 0 && g_orig_input_get_key_down) return g_orig_input_get_key_down(keycode);
  if(edge < 0 && g_orig_input_get_key_up) return g_orig_input_get_key_up(keycode);
  return 0;
}
static int my_input_get_key(int keycode){ return my_input_get_key_common(keycode, 0); }
static int my_input_get_key_down(int keycode){ return my_input_get_key_common(keycode, 1); }
static int my_input_get_key_up(int keycode){ return my_input_get_key_common(keycode, -1); }
/* ===== MOUSE VIRTUAL (navegacao do menu uGUI via EventSystem) =====
   /tmp/re4mouse contem "x y" (posicao do cursor, pixels topo-esquerda). Um clique e
   simulado por re4_mouse_click() (down N frames, up). O StandaloneInputModule do Unity
   le mousePosition + GetMouseButtonDown(0) -> raycast na UI -> aciona o botao sob o cursor. */
static float g_mouse_x=640.0f, g_mouse_y=360.0f;   /* pixels, origem topo-esquerda */
static int g_mouse_down=0, g_mouse_down_prev=0;
static int g_mouse_click_until=-1;
/* TOUCH DO GAMEPLAY (movimento do Leon): o dpad da tela le Input.GetTouch. O gamepad
   alimenta este "dedo virtual" sobre o dpad. Tem prioridade sobre o touch de debug. */
static float g_gp_tx=0, g_gp_ty=0; static int g_gp_tdown=0, g_gp_tprev=0;
static void re4_mouse_poll(void); /* fwd */
/* preenche o toque virtual ATIVO: gameplay tem prioridade; senao o debug (/tmp). */
static int re4_active_touch(float *x, float *y, int *down, int *prev){
  if(g_gp_tdown || g_gp_tprev){ *x=g_gp_tx; *y=g_gp_ty; *down=g_gp_tdown; *prev=g_gp_tprev; return 1; }
  re4_mouse_poll(); *x=g_mouse_x; *y=g_mouse_y; *down=g_mouse_down; *prev=g_mouse_down_prev;
  return (*down||*prev);
}
static void re4_mouse_poll(void){
  static int last=-999999;
  if(last==g_re4_frame) return; last=g_re4_frame;
  g_mouse_down_prev = g_mouse_down;
  /* le posicao do cursor (persistente) */
  FILE *f=fopen("/tmp/re4mouse","r");
  if(f){ float x,y; if(fscanf(f,"%f %f",&x,&y)==2 && x>=0){ g_mouse_x=x; g_mouse_y=y; } fclose(f); }
  /* clique pendente (arquivo gatilho /tmp/re4click) */
  f=fopen("/tmp/re4click","r");
  if(f){ int c=0; if(fscanf(f,"%d",&c)==1 && c>0){ fclose(f); f=fopen("/tmp/re4click","w"); if(f)fclose(f);
      g_mouse_click_until = g_re4_frame + re4_int_env("RE4_CLICK_HOLD",4,1,30);
    } else fclose(f); }
  g_mouse_down = (g_re4_frame <= g_mouse_click_until) ? 1 : 0;
}
static int my_input_get_mouse_button(int button){
  re4_mouse_poll();
  if(button==0) return g_mouse_down;
  return 0;
}
static int my_input_get_mouse_button_down(int button){
  re4_mouse_poll();
  if(button==0){ int r = g_mouse_down && !g_mouse_down_prev;
    if(r && getenv("RE4_GPLOG")){ fprintf(stderr,"[MOUSE] DOWN x=%.0f y=%.0f f=%d\n",g_mouse_x,g_mouse_y,g_re4_frame); fsync(2);} return r; }
  return 0;
}
static int my_input_get_mouse_button_up(int button){
  re4_mouse_poll();
  if(button==0) return !g_mouse_down && g_mouse_down_prev;
  return 0;
}
static int my_input_get_mouse_present(void){ return 1; }
/* INTERNAL_get_mousePosition(out Vector3): Unity usa origem BOTTOM-left -> y invertido. */
static void my_icall_get_mousePosition(void *out_vec3){
  re4_mouse_poll();
  if(!out_vec3) return;
  float *v=(float*)out_vec3;
  int h = re4_screen_height();
  v[0]=g_mouse_x;
  v[1]=(float)h - g_mouse_y;   /* inverte Y p/ coords do Unity */
  v[2]=0.0f;
}
/* touchCount: 1 SO quando injetamos movimento; senao 0 (NAO chamamos o original -> chamar o
   icall real de touch do Unity no menu/load TRAVAVA). g_orig_* mantido so p/ debug. */
static int my_input_get_touch_count(void){
  float x,y; int down,prev;
  return re4_active_touch(&x,&y,&down,&prev) ? 1 : 0;
}
/* INTERNAL_CALL_GetTouch(int index, out Touch). Layout Unity 2018 (ARM32, 4B campos):
   0:fingerId 4:pos.x 8:pos.y 12:rawPos.x 16:rawPos.y 20:dPos.x 24:dPos.y 28:dTime
   32:tapCount 36:phase 40:type 44:pressure ... Preenchemos o essencial p/ o EventSystem. */
static void my_icall_get_touch(int index, void *out_touch){
  if(!out_touch) return;
  float mx,my; int down,prev; re4_active_touch(&mx,&my,&down,&prev);
  /* NAO chamamos g_orig_gettouch (chamar o icall real no menu/load travava). Quando parado,
     touchCount=0 entao o jogo nem chama isto; se chamar, devolvemos toque zerado/inativo. */
  unsigned char *t=(unsigned char*)out_touch;
  memset(t, 0, 56);
  int h = re4_screen_height();
  float px=mx, py=(float)h - my;   /* Unity touch = bottom-left */
  int phase; /* 0=Began 1=Moved 2=Stationary 3=Ended 4=Canceled */
  if(down && !prev) phase=0;        /* Began */
  else if(!down && prev) phase=3;   /* Ended */
  else phase=1;                     /* Moved (joystick arrastando) */
  *(int*)(t+0)   = 0;        /* fingerId */
  *(float*)(t+4) = px;       /* position.x */
  *(float*)(t+8) = py;       /* position.y */
  *(float*)(t+12)= px;       /* rawPosition.x */
  *(float*)(t+16)= py;       /* rawPosition.y */
  *(float*)(t+20)= 0.0f;     /* deltaPosition.x */
  *(float*)(t+24)= 0.0f;     /* deltaPosition.y */
  *(float*)(t+28)= 0.016f;   /* deltaTime */
  *(int*)(t+32)  = 1;        /* tapCount */
  *(int*)(t+36)  = phase;    /* phase */
  *(int*)(t+40)  = 0;        /* type */
  *(float*)(t+44)= 1.0f;     /* pressure */
  *(float*)(t+48)= 1.0f;     /* maximumPossiblePressure */
  if(getenv("RE4_GPLOG") && phase!=2){ fprintf(stderr,"[TOUCHV] idx=%d phase=%d x=%.0f y=%.0f f=%d\n",index,phase,px,py,g_re4_frame); fsync(2); }
}
static int my_input_get_key_string_common(void *mono_string, int edge){
  char *name = re4_mono_string_to_utf8(mono_string);
  re4_gp_poll();
  if(getenv("RE4_INDUMP")){ char k[80]; snprintf(k,sizeof k,"%s(\"%s\")", edge==0?"GetKeyString":(edge>0?"GetKeyDownString":"GetKeyUpString"), name?name:"(null)"); re4_indump(k); }
  int id = re4_gp_button_id_from_name(name);
  if(getenv("RE4_GPTRACE")){
    static int tn = 0;
    if(tn++ < 240)
      fprintf(stderr, "[RTRACE] %s(\"%s\") id=%d f=%d\n",
              edge == 0 ? "GetKeyString" : (edge > 0 ? "GetKeyDownString" : "GetKeyUpString"),
              name ? name : "(null)", id, g_re4_frame);
  }
  if(id >= 0 && id < RE4_BTN_COUNT){
    int now = g_re4_gp_btn[id] ? 1 : 0;
    int prev = g_re4_gp_prev[id] ? 1 : 0;
    int r = edge == 0 ? now : (edge > 0 ? (now && !prev) : (!now && prev));
    if(getenv("RE4_GPLOG") && r){
      static int n = 0;
      if(n++ < 160){
        fprintf(stderr, "[RINPUT] %s(\"%s\") -> %d f=%d\n",
                edge == 0 ? "GetKeyString" : (edge > 0 ? "GetKeyDownString" : "GetKeyUpString"),
                name ? name : "(null)", r, g_re4_frame);
        fsync(2);
      }
    }
    re4_mono_free_utf8(name);
    return r;
  }
  re4_mono_free_utf8(name);
  if(edge == 0 && g_orig_input_get_key_string) return g_orig_input_get_key_string(mono_string);
  if(edge > 0 && g_orig_input_get_key_down_string) return g_orig_input_get_key_down_string(mono_string);
  if(edge < 0 && g_orig_input_get_key_up_string) return g_orig_input_get_key_up_string(mono_string);
  return 0;
}
static int my_input_get_key_string(void *mono_string){ return my_input_get_key_string_common(mono_string, 0); }
static int my_input_get_key_down_string(void *mono_string){ return my_input_get_key_string_common(mono_string, 1); }
static int my_input_get_key_up_string(void *mono_string){ return my_input_get_key_string_common(mono_string, -1); }
static int my_input_any_key(void){
  re4_gp_poll();
  for(int i = 0; i < RE4_BTN_COUNT; i++)
    if(g_re4_gp_btn[i]) return 1;
  return g_orig_input_any_key ? g_orig_input_any_key() : 0;
}
static int my_input_any_key_down(void){
  re4_gp_poll();
  for(int i = 0; i < RE4_BTN_COUNT; i++)
    if(g_re4_gp_btn[i] && !g_re4_gp_prev[i]) return 1;
  return g_orig_input_any_key_down ? g_orig_input_any_key_down() : 0;
}
typedef struct {
  uint32_t packet;
  uint16_t buttons;
  uint8_t lt, rt;
  int16_t lx, ly, rx, ry;
} re4_xinput_raw_t;
static void re4_xinput_fill(re4_xinput_raw_t *s){
  memset(s, 0, sizeof(*s));
  s->packet = (uint32_t)g_re4_gp_polls;
  if(g_re4_gp_btn[RE4_BTN_DU]) s->buttons |= 0x0001;
  if(g_re4_gp_btn[RE4_BTN_DD]) s->buttons |= 0x0002;
  if(g_re4_gp_btn[RE4_BTN_DL]) s->buttons |= 0x0004;
  if(g_re4_gp_btn[RE4_BTN_DR]) s->buttons |= 0x0008;
  if(g_re4_gp_btn[RE4_BTN_START]) s->buttons |= 0x0010;
  if(g_re4_gp_btn[RE4_BTN_BACK]) s->buttons |= 0x0020;
  if(g_re4_gp_btn[RE4_BTN_L3]) s->buttons |= 0x0040;
  if(g_re4_gp_btn[RE4_BTN_R3]) s->buttons |= 0x0080;
  if(g_re4_gp_btn[RE4_BTN_LB]) s->buttons |= 0x0100;
  if(g_re4_gp_btn[RE4_BTN_RB]) s->buttons |= 0x0200;
  if(g_re4_gp_btn[RE4_BTN_A]) s->buttons |= 0x1000;
  if(g_re4_gp_btn[RE4_BTN_B]) s->buttons |= 0x2000;
  if(g_re4_gp_btn[RE4_BTN_X]) s->buttons |= 0x4000;
  if(g_re4_gp_btn[RE4_BTN_Y]) s->buttons |= 0x8000;
  s->lt = (uint8_t)(g_re4_gp_lt * 255.0f);
  s->rt = (uint8_t)(g_re4_gp_rt * 255.0f);
  s->lx = (int16_t)(g_re4_gp_lx * 32767.0f);
  s->ly = (int16_t)(-g_re4_gp_ly * 32767.0f); /* XInput Y positivo para cima */
  s->rx = (int16_t)(g_re4_gp_rx * 32767.0f);
  s->ry = (int16_t)(-g_re4_gp_ry * 32767.0f);
}
static unsigned my_xinput_get_state(unsigned player_index, void *raw_state){
  (void)player_index;
  re4_gp_poll();
  if(!raw_state) return 1167u; /* ERROR_DEVICE_NOT_CONNECTED */
  re4_xinput_raw_t s;
  re4_xinput_fill(&s);
  memcpy(raw_state, &s, sizeof(s));
  return re4_gp_have_pad() ? 0u : 1167u;
}
static void my_xinput_set_state(unsigned player_index, float left_motor, float right_motor){
  (void)player_index; (void)left_motor; (void)right_motor;
}
/* SWAP UNICO POR FRAME: a Unity NAO chama eglSwapBuffers pelo shim; force_present e o unico
   present. Antes faziamos swap APOS CADA draw no FBO0 -> com double-buffer os 2 draws do composite
   caem em PAGINAS diferentes -> a pagina exibida nunca tem o frame completo (loading=1 draw=ok,
   menu=2 draws=quebra). Agora marcamos g_fbo0_dirty quando desenha no FBO0 e fazemos UM swap so,
   no inicio do proximo frame (quando o render volta a bindar um FBO offscreen). Assim todos os
   draws do FBO0 (composite full-screen) acumulam na MESMA pagina antes do swap. */
static volatile int g_fbo0_dirty=0;
/* textura-fonte do COMPOSITE: a que a Unity amostra no draw para o FBO0 (= a cena/menu final).
   g_fbo_color_tex[1] (id da color attachment do FBO1) NAO bate com a textura realmente amostrada
   (deu cyan). Capturamos a textura ligada na unidade ativa durante o draw no FBO0 e a reusamos
   no nosso re-blit no ponto do swap. */
static volatile unsigned g_composite_src_tex=0;
static unsigned g_gl_current_program=0;
static unsigned g_gl_active_texture=0x84c0; /* GL_TEXTURE0 */
static unsigned g_gl_bound_tex2d[4]={0,0,0,0};
static unsigned g_gl_bound_texext[4]={0,0,0,0};
static int g_gl_viewport[4]={0,0,0,0};
static int g_gl_blend_enabled=0;
static int g_gl_scissor_enabled=0;
static int g_gl_scissor[4]={0,0,0,0};
static unsigned g_gl_blend_src_rgb=1, g_gl_blend_dst_rgb=0, g_gl_blend_src_alpha=1, g_gl_blend_dst_alpha=0;
static unsigned char g_gl_color_mask[4]={1,1,1,1};
static unsigned g_fbo_color_tex[8]={0};
typedef struct {
  unsigned tex;
  int width;
  int height;
} re4_texdim_t;
static re4_texdim_t g_re4_texdims[128];
static unsigned g_re4_texdim_replace=0;
static int g_last_composite_log_frame=-1;
static unsigned g_re4_blit_program=0;
static int g_re4_blit_a_pos=-1, g_re4_blit_a_uv=-1, g_re4_blit_u_tex=-1, g_re4_blit_u_uvscale=-1;
static float g_snap_uvw=1.0f, g_snap_uvh=1.0f; /* fracao POT usada pela cena (NPOT->POT no Mali) */
static int g_snap_pot=0;                        /* lado POT alocado p/ g_snap_tex */
static int re4_screen_width(void);
static int re4_screen_height(void);
static unsigned g_frame_draws_fbo0=0, g_frame_draws_fbo1=0, g_frame_draws_fbo2=0, g_frame_draws_other=0;
static unsigned g_frame_clears_fbo0=0, g_frame_clears_fbo1=0, g_frame_clears_fbo2=0, g_frame_clears_other=0;
static int re4_texunit_index(unsigned unit){
  int idx=(int)(unit-0x84c0); /* GL_TEXTURE0 */
  return (idx>=0 && idx<(int)(sizeof(g_gl_bound_tex2d)/sizeof(g_gl_bound_tex2d[0]))) ? idx : -1;
}
static int re4_interesting_composite_frame(int frame){
  return frame>=420 && frame<=720;
}
static int re4_fix_composite_scissor_enabled(void){
  static int cached=-1;
  if(cached<0){
    const char *env=getenv("RE4_FIX_COMPOSITE_SCISSOR");
    cached=(!env || !env[0] || strcmp(env,"0")!=0) ? 1 : 0;
  }
  return cached;
}
static int re4_get_texture_dims(unsigned tex, int *width, int *height); /* fwd decl */
/* Loga os draws da CENA (FBO1) num frame alvo: programa + textura ligada (unidade ativa) + dims.
   Pra ver por que a cena 3D do menu sai PRETA (textura NPOT? tex=0? programa invalido?). */
static void re4_log_scene_draw(const char *kind, unsigned mode, int count){
  const char *fv=getenv("RE4_SCENE_LOG_FRAME"); int tf=(fv&&fv[0])?atoi(fv):-1;
  if(tf<0 || g_re4_frame<tf || g_gl_bound_fbo==0) return;
  static int total=0; if(total++>=60) return;       /* cap global */
  static int n=0; static int lastf=-1; if(lastf!=g_re4_frame){n=0;lastf=g_re4_frame;}
  if(n++>=8) return;                                  /* poucos por frame */
  int idx=re4_texunit_index(g_gl_active_texture);
  unsigned t=(idx>=0)?g_gl_bound_tex2d[idx]:0;
  int tw=0,th=0; int have=re4_get_texture_dims(t,&tw,&th);
  fprintf(stderr,"[SCENE] f=%d fbo=%u via=%s mode=0x%x count=%d prog=%u tex=%u dims=%dx%d%s blend=%d vp=%dx%d\n",
          g_re4_frame,g_gl_bound_fbo,kind?kind:"?",mode,count,g_gl_current_program,t,tw,th,
          have?"":"(?)",g_gl_blend_enabled,g_gl_viewport[2],g_gl_viewport[3]);
}
static void re4_log_composite_state(const char *kind, unsigned mode, int count){
  int idx=re4_texunit_index(g_gl_active_texture);
  if(g_gl_bound_fbo!=0 || !re4_interesting_composite_frame(g_re4_frame) || g_last_composite_log_frame==g_re4_frame)
    return;
  g_last_composite_log_frame=g_re4_frame;
  fprintf(stderr,
          "[COMPOSITE] f=%d via=%s mode=0x%x count=%d prog=%u vp=%d,%d %dx%d sc=%d %d,%d %dx%d blend=%d func=%u/%u/%u/%u mask=%u%u%u%u act=%u tex2d={%u,%u,%u,%u} texext={%u,%u,%u,%u} act2d=%u actext=%u fbo1tex=%u fbo2tex=%u\n",
          g_re4_frame, kind?kind:"?", mode, count, g_gl_current_program,
          g_gl_viewport[0], g_gl_viewport[1], g_gl_viewport[2], g_gl_viewport[3],
          g_gl_scissor_enabled, g_gl_scissor[0], g_gl_scissor[1], g_gl_scissor[2], g_gl_scissor[3],
          g_gl_blend_enabled,
          g_gl_blend_src_rgb, g_gl_blend_dst_rgb, g_gl_blend_src_alpha, g_gl_blend_dst_alpha,
          g_gl_color_mask[0], g_gl_color_mask[1], g_gl_color_mask[2], g_gl_color_mask[3],
          g_gl_active_texture,
          g_gl_bound_tex2d[0], g_gl_bound_tex2d[1], g_gl_bound_tex2d[2], g_gl_bound_tex2d[3],
          g_gl_bound_texext[0], g_gl_bound_texext[1], g_gl_bound_texext[2], g_gl_bound_texext[3],
          idx>=0?g_gl_bound_tex2d[idx]:0, idx>=0?g_gl_bound_texext[idx]:0,
          g_fbo_color_tex[1], g_fbo_color_tex[2]);
}
static int re4_begin_composite_scissor_override(const char *kind, int count, int *saved_enabled, int saved_rect[4]){
  int idx=re4_texunit_index(g_gl_active_texture);
  long long vp_area=(long long)g_gl_viewport[2]*(long long)g_gl_viewport[3];
  long long sc_area=(long long)g_gl_scissor[2]*(long long)g_gl_scissor[3];
  if(saved_enabled) *saved_enabled=g_gl_scissor_enabled;
  if(saved_rect){
    saved_rect[0]=g_gl_scissor[0];
    saved_rect[1]=g_gl_scissor[1];
    saved_rect[2]=g_gl_scissor[2];
    saved_rect[3]=g_gl_scissor[3];
  }
  if(!re4_fix_composite_scissor_enabled() || g_gl_bound_fbo!=0 || !g_gl_scissor_enabled || idx<0)
    return 0;
  if(!g_fbo_color_tex[1] || g_gl_bound_tex2d[idx]!=g_fbo_color_tex[1])
    return 0;
  if(g_gl_viewport[2]<=0 || g_gl_viewport[3]<=0 || g_gl_scissor[2]<=0 || g_gl_scissor[3]<=0)
    return 0;
  if(count>6)
    return 0;
  if(sc_area*16 > vp_area)
    return 0;
  if(!r_glDisable) r_glDisable=dlsym(RTLD_DEFAULT,"glDisable");
  if(!r_glDisable)
    return 0;
  r_glDisable(0x0c11); /* GL_SCISSOR_TEST */
  g_gl_scissor_enabled=0;
  fprintf(stderr,
          "[SCFIX] f=%d via=%s prog=%u tex=%u vp=%d,%d %dx%d sc=%d,%d %dx%d count=%d\n",
          g_re4_frame, kind?kind:"?", g_gl_current_program, g_gl_bound_tex2d[idx],
          g_gl_viewport[0], g_gl_viewport[1], g_gl_viewport[2], g_gl_viewport[3],
          g_gl_scissor[0], g_gl_scissor[1], g_gl_scissor[2], g_gl_scissor[3], count);
  return 1;
}
static void re4_end_composite_scissor_override(int active, int saved_enabled, const int saved_rect[4]){
  if(!active || !saved_enabled)
    return;
  if(!r_glEnable) r_glEnable=dlsym(RTLD_DEFAULT,"glEnable");
  if(!r_glScissor) r_glScissor=dlsym(RTLD_DEFAULT,"glScissor");
  if(r_glEnable)
    r_glEnable(0x0c11); /* GL_SCISSOR_TEST */
  g_gl_scissor_enabled=1;
  if(r_glScissor){
    r_glScissor(saved_rect[0],saved_rect[1],saved_rect[2],saved_rect[3]);
    g_gl_scissor[0]=saved_rect[0];
    g_gl_scissor[1]=saved_rect[1];
    g_gl_scissor[2]=saved_rect[2];
    g_gl_scissor[3]=saved_rect[3];
  }
}
static void re4_count_fbo_bucket(unsigned fbo, int is_clear){
  unsigned *slot = NULL;
  if (is_clear) {
    if (fbo == 0) slot = &g_frame_clears_fbo0;
    else if (fbo == 1) slot = &g_frame_clears_fbo1;
    else if (fbo == 2) slot = &g_frame_clears_fbo2;
    else slot = &g_frame_clears_other;
  } else {
    if (fbo == 0) slot = &g_frame_draws_fbo0;
    else if (fbo == 1) slot = &g_frame_draws_fbo1;
    else if (fbo == 2) slot = &g_frame_draws_fbo2;
    else slot = &g_frame_draws_other;
  }
  if (slot)
    (*slot)++;
}
static void re4_note_texture_dims(unsigned tex, int width, int height){
  int free_idx=-1;
  if(!tex || width<=0 || height<=0)
    return;
  for(int i=0;i<(int)(sizeof(g_re4_texdims)/sizeof(g_re4_texdims[0]));i++){
    if(g_re4_texdims[i].tex==tex){
      g_re4_texdims[i].width=width;
      g_re4_texdims[i].height=height;
      return;
    }
    if(free_idx<0 && g_re4_texdims[i].tex==0)
      free_idx=i;
  }
  if(free_idx<0)
    free_idx=(int)(g_re4_texdim_replace++ % (sizeof(g_re4_texdims)/sizeof(g_re4_texdims[0])));
  g_re4_texdims[free_idx].tex=tex;
  g_re4_texdims[free_idx].width=width;
  g_re4_texdims[free_idx].height=height;
}
static int re4_get_texture_dims(unsigned tex, int *width, int *height){
  if(!tex)
    return 0;
  for(int i=0;i<(int)(sizeof(g_re4_texdims)/sizeof(g_re4_texdims[0]));i++){
    if(g_re4_texdims[i].tex==tex){
      if(width) *width=g_re4_texdims[i].width;
      if(height) *height=g_re4_texdims[i].height;
      return 1;
    }
  }
  return 0;
}
static const char *re4_logstamp(void){
  const char *stamp=getenv("RE4_LOGSTAMP");
  return (stamp&&stamp[0])?stamp:"manual";
}
static void re4_ensure_logdir(void){
  char path[512];
  snprintf(path,sizeof(path),"%s/logs",re4_gamedir());
  mkdir(path,0755);
  snprintf(path,sizeof(path),"%s/logs/%s",re4_gamedir(),re4_logstamp());
  mkdir(path,0755);
}
static int re4_write_full(int fd, const void *buf, size_t size){
  const unsigned char *p=(const unsigned char*)buf;
  while(size>0){
    ssize_t wr=write(fd,p,size);
    if(wr<0){
      if(errno==EINTR) continue;
      return -1;
    }
    p+=wr;
    size-=wr;
  }
  return 0;
}
static void re4_dump_rgba_file(const char *tag, int frame, int width, int height, const unsigned char *pixels){
  char path[512];
  size_t count=(size_t)width*(size_t)height;
  size_t nonblack=0;
  size_t center_idx;
  const unsigned char *center;
  int fd;
  if(!tag || !pixels || width<=0 || height<=0)
    return;
  re4_ensure_logdir();
  snprintf(path,sizeof(path),"%s/logs/%s/%s-%d-%dx%d.rgba",
           re4_gamedir(),re4_logstamp(),tag,frame,width,height);
  fd=open(path,O_CREAT|O_TRUNC|O_WRONLY,0644);
  if(fd<0){
    fprintf(stderr,"[FBDUMP] open fail %s: %s\n",path,strerror(errno));
    return;
  }
  if(re4_write_full(fd,pixels,count*4)!=0){
    fprintf(stderr,"[FBDUMP] write fail %s: %s\n",path,strerror(errno));
    close(fd);
    return;
  }
  close(fd);
  for(size_t i=0;i<count;i++){
    const unsigned char *px=pixels + (i*4);
    if(px[0] || px[1] || px[2] || px[3])
      nonblack++;
  }
  center_idx=((size_t)(height/2)*(size_t)width + (size_t)(width/2))*4;
  center=pixels + center_idx;
  fprintf(stderr,
          "[FBDUMP] wrote %s nonblack=%zu/%zu center=%u,%u,%u,%u\n",
          path,nonblack,count,center[0],center[1],center[2],center[3]);
}
static void re4_dump_bound_framebuffer(const char *tag, unsigned fbo, int width, int height){
  size_t size;
  unsigned char *pixels;
  if(width<=0 || height<=0 || !r_glReadPixels || !r_glBindFramebuffer)
    return;
  size=(size_t)width*(size_t)height*4;
  pixels=(unsigned char*)malloc(size);
  if(!pixels){
    fprintf(stderr,"[FBDUMP] alloc fail tag=%s size=%zu\n",tag?tag:"?",size);
    return;
  }
  memset(pixels,0,size);
  r_glBindFramebuffer(0x8d40,fbo);
  g_gl_bound_fbo=fbo;
  r_glReadPixels(0,0,width,height,0x1908,0x1401,pixels); /* GL_RGBA / GL_UNSIGNED_BYTE */
  re4_dump_rgba_file(tag,g_re4_frame,width,height,pixels);
  free(pixels);
}
static void re4_dump_window_draw_if_needed(const char *tag){
  static int dump_frame=-2;
  static int dumped=0;
  size_t size;
  unsigned char *pixels;
  int width=g_gl_viewport[2];
  int height=g_gl_viewport[3];
  if(dump_frame==-2){
    const char *value=getenv("RE4_DUMP_WINDOW_FRAME");
    dump_frame=(value&&value[0])?atoi(value):-1;
  }
  if(dumped || dump_frame<0 || g_re4_frame!=dump_frame || g_gl_bound_fbo!=0)
    return;
  if(width<=0 || height<=0)
    return;
  if(!r_glReadPixels) r_glReadPixels=dlsym(RTLD_DEFAULT,"glReadPixels");
  if(!r_glReadPixels)
    return;
  size=(size_t)width*(size_t)height*4;
  pixels=(unsigned char*)malloc(size);
  if(!pixels){
    fprintf(stderr,"[WNDUMP] alloc fail size=%zu\n",size);
    return;
  }
  memset(pixels,0,size);
  r_glReadPixels(0,0,width,height,0x1908,0x1401,pixels);
  re4_dump_rgba_file(tag?tag:"win",g_re4_frame,width,height,pixels);
  fprintf(stderr,"[WNDUMP] frame=%d tag=%s vp=%d,%d %dx%d\n",
          g_re4_frame,tag?tag:"win",g_gl_viewport[0],g_gl_viewport[1],width,height);
  free(pixels);
  dumped=1;
}
/* RE4_DUMP_FBO_FRAMES="900,1500,2500" -> dumpa o FBO atual em cada frame (multi-shot).
   Tem prioridade sobre RE4_DUMP_FBO_FRAME (single). Retorna 1 se deve dumpar neste frame. */
static int re4_multi_dump_hit(int frame){
  static int parsed=0; static int frames[32]; static int n=0;
  if(!parsed){ parsed=1; const char *s=getenv("RE4_DUMP_FBO_FRAMES");
    if(s&&s[0]){ const char *p=s; while(*p&&n<32){ frames[n++]=atoi(p); const char*c=strchr(p,','); if(!c)break; p=c+1; } } }
  for(int i=0;i<n;i++) if(frames[i]==frame) return 1;
  return 0;
}
static void re4_dump_framebuffers_if_needed(int frame){
  static int dump_frame=-2;
  static int dumped=0;
  if(re4_multi_dump_hit(frame)){
    /* multi-shot: dumpa o FBO atualmente ligado (= o que esta sendo composto/apresentado) */
    unsigned sv=g_gl_bound_fbo;
    int w=(g_gl_viewport[2]>0)?g_gl_viewport[2]:re4_screen_width();
    int h=(g_gl_viewport[3]>0)?g_gl_viewport[3]:re4_screen_height();
    if(!r_glReadPixels) r_glReadPixels=dlsym(RTLD_DEFAULT,"glReadPixels");
    if(!r_glBindFramebuffer) r_glBindFramebuffer=dlsym(RTLD_DEFAULT,"glBindFramebuffer");
    if(r_glReadPixels&&r_glBindFramebuffer){
      char tag[16]; snprintf(tag,sizeof tag,"cur");
      re4_dump_bound_framebuffer(tag, sv?sv:1, w, h);   /* sv 0 cedo? usa fbo1 */
    }
    return;
  }
  unsigned saved_fbo=g_gl_bound_fbo;
  int cur_w=(g_gl_viewport[2]>0)?g_gl_viewport[2]:re4_screen_width();
  int cur_h=(g_gl_viewport[3]>0)?g_gl_viewport[3]:re4_screen_height();
  int screen_w=re4_screen_width();
  int screen_h=re4_screen_height();
  int fbo1_w=cur_w, fbo1_h=cur_h;
  int fbo2_w=cur_w, fbo2_h=cur_h;
  if(dump_frame==-2){
    const char *value=getenv("RE4_DUMP_FBO_FRAME");
    dump_frame=(value&&value[0])?atoi(value):-1;
  }
  if(dumped || dump_frame<0 || frame!=dump_frame)
    return;
  if(!r_glReadPixels) r_glReadPixels=dlsym(RTLD_DEFAULT,"glReadPixels");
  if(!r_glBindFramebuffer) r_glBindFramebuffer=dlsym(RTLD_DEFAULT,"glBindFramebuffer");
  if(!r_glReadPixels || !r_glBindFramebuffer)
    return;
  if(g_fbo_color_tex[1])
    re4_get_texture_dims(g_fbo_color_tex[1],&fbo1_w,&fbo1_h);
  if(g_fbo_color_tex[2])
    re4_get_texture_dims(g_fbo_color_tex[2],&fbo2_w,&fbo2_h);
  fprintf(stderr,
          "[FBDUMP] begin f=%d saved=%u curvp=%d,%d %dx%d fbo1tex=%u(%dx%d) fbo2tex=%u(%dx%d)\n",
          frame,saved_fbo,g_gl_viewport[0],g_gl_viewport[1],cur_w,cur_h,
          g_fbo_color_tex[1],fbo1_w,fbo1_h,g_fbo_color_tex[2],fbo2_w,fbo2_h);
  re4_dump_bound_framebuffer(saved_fbo?"cur":"cur0",saved_fbo,saved_fbo?cur_w:screen_w,saved_fbo?cur_h:screen_h);
  if(saved_fbo!=1 && g_fbo_color_tex[1])
    re4_dump_bound_framebuffer("fbo1",1,fbo1_w,fbo1_h);
  if(saved_fbo!=2 && g_fbo_color_tex[2])
    re4_dump_bound_framebuffer("fbo2",2,fbo2_w,fbo2_h);
  re4_dump_bound_framebuffer("def",0,screen_w,screen_h);
  if(saved_fbo!=g_gl_bound_fbo){
    r_glBindFramebuffer(0x8d40,saved_fbo);
    g_gl_bound_fbo=saved_fbo;
  }
  dumped=1;
}
static void re4_log_and_reset_fbo_stats(int frame){
  int interesting = (frame < 8) || (frame % 60 == 0) ||
                    (frame >= 430 && frame <= 560 && g_frame_draws_fbo0 == 0);
  if (interesting) {
    fprintf(stderr,
            "[FBOSTAT] f=%d cur=%u draw0=%u draw1=%u draw2=%u drawX=%u clear0=%u clear1=%u clear2=%u clearX=%u\n",
            frame, g_gl_bound_fbo,
            g_frame_draws_fbo0, g_frame_draws_fbo1, g_frame_draws_fbo2, g_frame_draws_other,
            g_frame_clears_fbo0, g_frame_clears_fbo1, g_frame_clears_fbo2, g_frame_clears_other);
  }
  g_frame_draws_fbo0 = g_frame_draws_fbo1 = g_frame_draws_fbo2 = g_frame_draws_other = 0;
  g_frame_clears_fbo0 = g_frame_clears_fbo1 = g_frame_clears_fbo2 = g_frame_clears_other = 0;
}
static void re4_probe_frame_pixels(int frame){
  unsigned char cur_px[4] = {0, 0, 0, 0};
  unsigned char def_px[4] = {0, 0, 0, 0};
  unsigned saved_fbo = g_gl_bound_fbo;
  int probe = (frame == 120 || frame == 240 || frame == 480 || frame == 540);
  if (!probe)
    return;
  if (!r_glReadPixels)
    r_glReadPixels = dlsym(RTLD_DEFAULT, "glReadPixels");
  if (!r_glReadPixels || !r_glBindFramebuffer)
    return;
  int cx = re4_screen_width() / 2;
  int cy = re4_screen_height() / 2;
  r_glReadPixels(cx, cy, 1, 1, 0x1908, 0x1401, cur_px); /* GL_RGBA / GL_UNSIGNED_BYTE */
  if (saved_fbo != 0)
    r_glBindFramebuffer(0x8d40, 0);
  r_glReadPixels(cx, cy, 1, 1, 0x1908, 0x1401, def_px);
  if (saved_fbo != 0)
    r_glBindFramebuffer(0x8d40, saved_fbo);
  fprintf(stderr,
          "[FBPROBE] f=%d curfbo=%u cur=%u,%u,%u,%u def=%u,%u,%u,%u\n",
          frame, saved_fbo,
          cur_px[0], cur_px[1], cur_px[2], cur_px[3],
          def_px[0], def_px[1], def_px[2], def_px[3]);
}
static int re4_ensure_blit_program(void){
  static const char *vs_src =
      "attribute vec2 a_pos;\n"
      "uniform vec2 u_uvscale;\n"
      "varying vec2 v_uv;\n"
      "void main(void){\n"
      "  v_uv = (a_pos * 0.5 + 0.5) * u_uvscale;\n" /* UV da posicao, escalada p/ sub-regiao POT */
      "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
      "}\n";
  static const char *fs_tex =
      "precision mediump float;\n"
      "varying vec2 v_uv;\n"
      "uniform sampler2D u_tex;\n"
      "void main(void){\n"
      "  gl_FragColor = texture2D(u_tex, v_uv);\n"
      "}\n";
  static const char *fs_dbg = /* DEBUG: mostra a UV como gradiente (R=u,G=v) -> testa shader/UV */
      "precision mediump float;\n"
      "varying vec2 v_uv;\n"
      "void main(void){\n"
      "  gl_FragColor = vec4(v_uv, 0.0, 1.0);\n"
      "}\n";
  const char *fs_src = getenv("RE4_BLIT_DBG") ? fs_dbg : fs_tex;
  int ok=0;
  unsigned vs=0, fs=0, prog=0;
  char logbuf[256];
  if(g_re4_blit_program)
    return 1;
  if(!r_glCreateShader) r_glCreateShader=dlsym(RTLD_DEFAULT,"glCreateShader");
  if(!r_glCreateProgram) r_glCreateProgram=dlsym(RTLD_DEFAULT,"glCreateProgram");
  if(!r_glAttachShader) r_glAttachShader=dlsym(RTLD_DEFAULT,"glAttachShader");
  if(!r_glLinkProgram) r_glLinkProgram=dlsym(RTLD_DEFAULT,"glLinkProgram");
  if(!r_glGetProgramiv) r_glGetProgramiv=dlsym(RTLD_DEFAULT,"glGetProgramiv");
  if(!r_glGetProgramInfoLog) r_glGetProgramInfoLog=dlsym(RTLD_DEFAULT,"glGetProgramInfoLog");
  if(!r_glGetUniformLocation) r_glGetUniformLocation=dlsym(RTLD_DEFAULT,"glGetUniformLocation");
  if(!r_glGetAttribLocation) r_glGetAttribLocation=dlsym(RTLD_DEFAULT,"glGetAttribLocation");
  if(!r_glUniform1i) r_glUniform1i=dlsym(RTLD_DEFAULT,"glUniform1i");
  if(!r_glEnableVertexAttribArray) r_glEnableVertexAttribArray=dlsym(RTLD_DEFAULT,"glEnableVertexAttribArray");
  if(!r_glDisableVertexAttribArray) r_glDisableVertexAttribArray=dlsym(RTLD_DEFAULT,"glDisableVertexAttribArray");
  if(!r_glVertexAttribPointer) r_glVertexAttribPointer=dlsym(RTLD_DEFAULT,"glVertexAttribPointer");
  if(!r_glDeleteShader) r_glDeleteShader=dlsym(RTLD_DEFAULT,"glDeleteShader");
  if(!r_glShaderSource) r_glShaderSource=dlsym(RTLD_DEFAULT,"glShaderSource");
  if(!r_glCompileShader) r_glCompileShader=dlsym(RTLD_DEFAULT,"glCompileShader");
  if(!r_glGetShaderiv) r_glGetShaderiv=dlsym(RTLD_DEFAULT,"glGetShaderiv");
  if(!r_glGetShaderInfoLog) r_glGetShaderInfoLog=dlsym(RTLD_DEFAULT,"glGetShaderInfoLog");
  if(!r_glCreateShader || !r_glCreateProgram || !r_glAttachShader || !r_glLinkProgram ||
     !r_glGetProgramiv || !r_glGetUniformLocation || !r_glGetAttribLocation ||
     !r_glUniform1i || !r_glEnableVertexAttribArray || !r_glVertexAttribPointer ||
     !r_glShaderSource || !r_glCompileShader || !r_glGetShaderiv){
    fprintf(stderr,"[FORCEBLIT] missing GL symbols\n");
    return 0;
  }
  vs=r_glCreateShader(0x8b31); /* GL_VERTEX_SHADER */
  fs=r_glCreateShader(0x8b30); /* GL_FRAGMENT_SHADER */
  r_glShaderSource(vs,1,&vs_src,NULL);
  r_glCompileShader(vs);
  r_glGetShaderiv(vs,0x8b81,&ok); /* GL_COMPILE_STATUS */
  if(!ok){
    logbuf[0]=0;
    if(r_glGetShaderInfoLog) r_glGetShaderInfoLog(vs,sizeof(logbuf)-1,NULL,logbuf);
    fprintf(stderr,"[FORCEBLIT] vertex compile fail: %s\n",logbuf);
    return 0;
  }
  r_glShaderSource(fs,1,&fs_src,NULL);
  r_glCompileShader(fs);
  r_glGetShaderiv(fs,0x8b81,&ok);
  if(!ok){
    logbuf[0]=0;
    if(r_glGetShaderInfoLog) r_glGetShaderInfoLog(fs,sizeof(logbuf)-1,NULL,logbuf);
    fprintf(stderr,"[FORCEBLIT] fragment compile fail: %s\n",logbuf);
    return 0;
  }
  prog=r_glCreateProgram();
  r_glAttachShader(prog,vs);
  r_glAttachShader(prog,fs);
  r_glLinkProgram(prog);
  r_glGetProgramiv(prog,0x8b82,&ok); /* GL_LINK_STATUS */
  if(!ok){
    logbuf[0]=0;
    if(r_glGetProgramInfoLog) r_glGetProgramInfoLog(prog,sizeof(logbuf)-1,NULL,logbuf);
    fprintf(stderr,"[FORCEBLIT] link fail: %s\n",logbuf);
    return 0;
  }
  if(r_glDeleteShader){ r_glDeleteShader(vs); r_glDeleteShader(fs); }
  g_re4_blit_program=prog;
  g_re4_blit_a_pos=r_glGetAttribLocation(prog,"a_pos");
  g_re4_blit_a_uv=r_glGetAttribLocation(prog,"a_uv");
  g_re4_blit_u_tex=r_glGetUniformLocation(prog,"u_tex");
  g_re4_blit_u_uvscale=r_glGetUniformLocation(prog,"u_uvscale");
  fprintf(stderr,"[FORCEBLIT] ready prog=%u a_pos=%d a_uv=%d u_tex=%d\n",
          g_re4_blit_program,g_re4_blit_a_pos,g_re4_blit_a_uv,g_re4_blit_u_tex);
  return 1;
}
static void re4_force_fbo1_blit_if_needed(int frame){
  static int enabled=-1;
  static const float verts[] = {
    -1.0f, -1.0f, 0.0f, 0.0f,
     1.0f, -1.0f, 1.0f, 0.0f,
     1.0f,  1.0f, 1.0f, 1.0f,
    -1.0f, -1.0f, 0.0f, 0.0f,
     1.0f,  1.0f, 1.0f, 1.0f,
    -1.0f,  1.0f, 0.0f, 1.0f,
  };
  unsigned saved_fbo=g_gl_bound_fbo;
  unsigned saved_program=g_gl_current_program;
  unsigned saved_active=g_gl_active_texture;
  int saved_idx=re4_texunit_index(saved_active);
  unsigned saved_tex2d=(saved_idx>=0)?g_gl_bound_tex2d[saved_idx]:0;
  int saved_vp[4]={g_gl_viewport[0],g_gl_viewport[1],g_gl_viewport[2],g_gl_viewport[3]};
  if(enabled==-1) enabled=getenv("RE4_FORCE_FBO1_BLIT")?1:0;
  if(!enabled || !g_fbo_color_tex[1])
    return;
  if(!re4_ensure_blit_program())
    return;
  if(!r_glBindFramebuffer) r_glBindFramebuffer=dlsym(RTLD_DEFAULT,"glBindFramebuffer");
  if(!r_glDrawArrays) r_glDrawArrays=dlsym(RTLD_DEFAULT,"glDrawArrays");
  if(!r_glUseProgram) r_glUseProgram=dlsym(RTLD_DEFAULT,"glUseProgram");
  if(!r_glActiveTexture) r_glActiveTexture=dlsym(RTLD_DEFAULT,"glActiveTexture");
  if(!r_glBindTexture) r_glBindTexture=dlsym(RTLD_DEFAULT,"glBindTexture");
  if(!r_glViewport) r_glViewport=dlsym(RTLD_DEFAULT,"glViewport");
  if(!r_glDisable) r_glDisable=dlsym(RTLD_DEFAULT,"glDisable");
  if(!r_glColorMask) r_glColorMask=dlsym(RTLD_DEFAULT,"glColorMask");
  if(!r_glUniform1i || !r_glEnableVertexAttribArray || !r_glVertexAttribPointer || !r_glDrawArrays)
    return;
  r_glBindFramebuffer(0x8d40,0);
  g_gl_bound_fbo=0;
  r_glViewport(0,0,re4_screen_width(),re4_screen_height());
  g_gl_viewport[0]=0; g_gl_viewport[1]=0; g_gl_viewport[2]=re4_screen_width(); g_gl_viewport[3]=re4_screen_height();
  r_glDisable(0x0be2); /* GL_BLEND */
  r_glDisable(0x0b44); /* GL_CULL_FACE */
  r_glDisable(0x0b71); /* GL_DEPTH_TEST */
  r_glDisable(0x0c11); /* GL_SCISSOR_TEST */
  g_gl_blend_enabled=0;
  r_glColorMask(1,1,1,1);
  g_gl_color_mask[0]=g_gl_color_mask[1]=g_gl_color_mask[2]=g_gl_color_mask[3]=1;
  r_glUseProgram(g_re4_blit_program);
  g_gl_current_program=g_re4_blit_program;
  r_glActiveTexture(0x84c0);
  g_gl_active_texture=0x84c0;
  r_glBindTexture(0x0de1,g_fbo_color_tex[1]);
  g_gl_bound_tex2d[0]=g_fbo_color_tex[1];
  r_glUniform1i(g_re4_blit_u_tex,0);
  r_glEnableVertexAttribArray((unsigned)g_re4_blit_a_pos);
  r_glEnableVertexAttribArray((unsigned)g_re4_blit_a_uv);
  r_glVertexAttribPointer((unsigned)g_re4_blit_a_pos,2,0x1406,0,4*sizeof(float),verts);
  r_glVertexAttribPointer((unsigned)g_re4_blit_a_uv,2,0x1406,0,4*sizeof(float),verts+2);
  r_glDrawArrays(0x0004,0,6); /* GL_TRIANGLES */
  if(frame<8 || frame%60==0 || frame>=420)
    fprintf(stderr,"[FORCEBLIT] f=%d tex=%u screen=%dx%d\n",
            frame,g_fbo_color_tex[1],re4_screen_width(),re4_screen_height());
  if(r_glDisableVertexAttribArray){
    r_glDisableVertexAttribArray((unsigned)g_re4_blit_a_pos);
    r_glDisableVertexAttribArray((unsigned)g_re4_blit_a_uv);
  }
  if(saved_fbo!=0 && r_glBindFramebuffer){
    r_glBindFramebuffer(0x8d40,saved_fbo);
    g_gl_bound_fbo=saved_fbo;
  }
  if(r_glUseProgram){
    r_glUseProgram(saved_program);
    g_gl_current_program=saved_program;
  }
  if(r_glActiveTexture){
    r_glActiveTexture(saved_active);
    g_gl_active_texture=saved_active;
  }
  if(saved_idx>=0 && r_glBindTexture){
    r_glBindTexture(0x0de1,saved_tex2d);
    g_gl_bound_tex2d[saved_idx]=saved_tex2d;
  }
  if(r_glViewport){
    r_glViewport(saved_vp[0],saved_vp[1],saved_vp[2],saved_vp[3]);
    g_gl_viewport[0]=saved_vp[0]; g_gl_viewport[1]=saved_vp[1];
    g_gl_viewport[2]=saved_vp[2]; g_gl_viewport[3]=saved_vp[3];
  }
}
/* BLIT na RENDER THREAD: copia g_fbo_color_tex[1] (a cena/menu que a Unity renderiza offscreen)
   para o FBO0 (window back buffer) IMEDIATAMENTE antes do swap, na propria render thread, sem
   release de contexto no meio. Diagnostico mostrou: present OK (clear vermelho chega ao fb0), mas
   o composite da Unity some entre o draw e o scanout -> refazemos o composite aqui com shader
   conhecido. Salva/restaura o minimo (ARRAY_BUFFER + attribs) pra nao corromper o frame seguinte. */
static int re4_rt_blit_fbo1(void){
  static const float verts[] = {
    -1.0f,-1.0f, 0.0f,0.0f,   1.0f,-1.0f, 1.0f,0.0f,   1.0f, 1.0f, 1.0f,1.0f,
    -1.0f,-1.0f, 0.0f,0.0f,   1.0f, 1.0f, 1.0f,1.0f,  -1.0f, 1.0f, 0.0f,1.0f,
  };
  unsigned src=g_snap_tex?g_snap_tex:(g_composite_src_tex?g_composite_src_tex:g_fbo_color_tex[1]);
  /* TESTTEX: cria uma textura 2x2 CONHECIDA (R,G,B,W) e blita -> se aparecer 4 quadrantes, o
     sampling do meu blit funciona (problema=textura fonte); se sair solido, o sampling esta quebrado. */
  if(getenv("RE4_BLIT_TESTTEX")){
    static unsigned ttex=0;
    if(!ttex){
      if(!r_glGenTextures) r_glGenTextures=dlsym(RTLD_DEFAULT,"glGenTextures");
      if(!r_glTexImage2D) r_glTexImage2D=dlsym(RTLD_DEFAULT,"glTexImage2D");
      if(!r_glActiveTexture) r_glActiveTexture=dlsym(RTLD_DEFAULT,"glActiveTexture");
      if(!r_glBindTexture) r_glBindTexture=dlsym(RTLD_DEFAULT,"glBindTexture");
      if(!r_glTexParameteri) r_glTexParameteri=dlsym(RTLD_DEFAULT,"glTexParameteri");
      unsigned char tx[16]={255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,255,255};
      if(r_glGenTextures&&r_glTexImage2D){ r_glActiveTexture(0x84c0); r_glGenTextures(1,&ttex);
        r_glBindTexture(0x0de1,ttex);
        r_glTexImage2D(0x0de1,0,0x1908,2,2,0,0x1908,0x1401,tx);
        r_glTexParameteri(0x0de1,0x2801,0x2600); r_glTexParameteri(0x0de1,0x2800,0x2600); /* NEAREST */
        r_glTexParameteri(0x0de1,0x2802,0x812f); r_glTexParameteri(0x0de1,0x2803,0x812f); }
    }
    if(ttex) src=ttex;
  }
  if(!src) return 0;
  if(!re4_ensure_blit_program()) return 0;
  if(!r_glBindFramebuffer) r_glBindFramebuffer=dlsym(RTLD_DEFAULT,"glBindFramebuffer");
  if(!r_glDrawArrays) r_glDrawArrays=dlsym(RTLD_DEFAULT,"glDrawArrays");
  if(!r_glUseProgram) r_glUseProgram=dlsym(RTLD_DEFAULT,"glUseProgram");
  if(!r_glActiveTexture) r_glActiveTexture=dlsym(RTLD_DEFAULT,"glActiveTexture");
  if(!r_glBindTexture) r_glBindTexture=dlsym(RTLD_DEFAULT,"glBindTexture");
  if(!r_glViewport) r_glViewport=dlsym(RTLD_DEFAULT,"glViewport");
  if(!r_glDisable) r_glDisable=dlsym(RTLD_DEFAULT,"glDisable");
  if(!r_glColorMask) r_glColorMask=dlsym(RTLD_DEFAULT,"glColorMask");
  if(!r_glGetIntegerv) r_glGetIntegerv=dlsym(RTLD_DEFAULT,"glGetIntegerv");
  if(!r_glBindBuffer) r_glBindBuffer=dlsym(RTLD_DEFAULT,"glBindBuffer");
  if(!r_glBindFramebuffer||!r_glDrawArrays||!r_glUseProgram||!r_glActiveTexture||!r_glBindTexture||
     !r_glViewport||!r_glUniform1i||!r_glEnableVertexAttribArray||!r_glVertexAttribPointer)
    return 0;
  if(!r_glGetIntegerv) r_glGetIntegerv=dlsym(RTLD_DEFAULT,"glGetIntegerv");
  if(!r_glIsEnabled) r_glIsEnabled=dlsym(RTLD_DEFAULT,"glIsEnabled");
  if(!r_glGetBooleanv) r_glGetBooleanv=dlsym(RTLD_DEFAULT,"glGetBooleanv");
  if(!r_glEnable) r_glEnable=dlsym(RTLD_DEFAULT,"glEnable");
  if(!r_glTexParameteri) r_glTexParameteri=dlsym(RTLD_DEFAULT,"glTexParameteri");
  /* === SAVE estado real (glGet) p/ restaurar e NAO corromper o frame seguinte da Unity === */
  int s_prog=0,s_vp[4]={0,0,0,0},s_act=0x84c0,s_tex0=0,s_arraybuf=0,s_fbo=0;
  unsigned char s_blend=0,s_cull=0,s_depth=0,s_scis=0,s_cmask[4]={1,1,1,1};
  if(r_glGetIntegerv){
    r_glGetIntegerv(0x8b8d,&s_prog);              /* CURRENT_PROGRAM */
    r_glGetIntegerv(0x0ba2,s_vp);                 /* VIEWPORT */
    r_glGetIntegerv(0x84e0,&s_act);               /* ACTIVE_TEXTURE */
    r_glGetIntegerv(0x8894,&s_arraybuf);          /* ARRAY_BUFFER_BINDING */
    r_glGetIntegerv(0x8ca6,&s_fbo);               /* FRAMEBUFFER_BINDING */
  }
  if(r_glActiveTexture) r_glActiveTexture(0x84c0);
  if(r_glGetIntegerv) r_glGetIntegerv(0x8069,&s_tex0); /* TEXTURE_BINDING_2D (unidade 0) */
  if(r_glIsEnabled){ s_blend=r_glIsEnabled(0x0be2); s_cull=r_glIsEnabled(0x0b44);
                     s_depth=r_glIsEnabled(0x0b71); s_scis=r_glIsEnabled(0x0c11); }
  if(r_glGetBooleanv) r_glGetBooleanv(0x0c23,s_cmask); /* COLOR_WRITEMASK */
  if(r_glBindBuffer) r_glBindBuffer(0x8892,0);
  /* === BLIT === */
  r_glBindFramebuffer(0x8d40,0);
  r_glViewport(0,0,re4_screen_width(),re4_screen_height());
  if(r_glDisable){ r_glDisable(0x0be2); r_glDisable(0x0b44); r_glDisable(0x0b71); r_glDisable(0x0c11); }
  if(r_glColorMask) r_glColorMask(1,1,1,1);
  r_glUseProgram(g_re4_blit_program);
  r_glBindTexture(0x0de1,src);
  if(r_glTexParameteri){
    r_glTexParameteri(0x0de1,0x2801,0x2601); /* MIN_FILTER=LINEAR */
    r_glTexParameteri(0x0de1,0x2800,0x2601); /* MAG_FILTER=LINEAR */
    r_glTexParameteri(0x0de1,0x2802,0x812f); /* WRAP_S=CLAMP_TO_EDGE */
    r_glTexParameteri(0x0de1,0x2803,0x812f); /* WRAP_T=CLAMP_TO_EDGE */
  }
  r_glUniform1i(g_re4_blit_u_tex,0);
  /* u_uvscale: se a fonte e o snapshot POT, escala a UV pra sub-regiao da cena; senao 1,1 */
  if(!r_glUniform2f) r_glUniform2f=dlsym(RTLD_DEFAULT,"glUniform2f");
  if(r_glUniform2f && g_re4_blit_u_uvscale>=0){
    if(src==g_snap_tex) r_glUniform2f(g_re4_blit_u_uvscale,g_snap_uvw,g_snap_uvh);
    else                r_glUniform2f(g_re4_blit_u_uvscale,1.0f,1.0f);
  }
  r_glEnableVertexAttribArray((unsigned)g_re4_blit_a_pos);
  r_glVertexAttribPointer((unsigned)g_re4_blit_a_pos,2,0x1406,0,4*sizeof(float),verts); /* UV derivada no VS */
  r_glDrawArrays(0x0004,0,6);
  if(r_glDisableVertexAttribArray){
    r_glDisableVertexAttribArray((unsigned)g_re4_blit_a_pos);
  }
  /* === RESTORE estado real === */
  r_glBindTexture(0x0de1,(unsigned)s_tex0);
  if(r_glActiveTexture) r_glActiveTexture((unsigned)s_act);
  r_glUseProgram((unsigned)s_prog);
  r_glViewport(s_vp[0],s_vp[1],s_vp[2],s_vp[3]);
  if(r_glEnable&&r_glDisable){
    (s_blend?r_glEnable:r_glDisable)(0x0be2);
    (s_cull ?r_glEnable:r_glDisable)(0x0b44);
    (s_depth?r_glEnable:r_glDisable)(0x0b71);
    (s_scis ?r_glEnable:r_glDisable)(0x0c11);
  }
  if(r_glColorMask) r_glColorMask(s_cmask[0],s_cmask[1],s_cmask[2],s_cmask[3]);
  if(r_glBindBuffer) r_glBindBuffer(0x8892,(unsigned)s_arraybuf);
  r_glBindFramebuffer(0x8d40,(unsigned)s_fbo);
  static int lg=0; if(lg++<8) fprintf(stderr,"[RT_BLIT] f=%d blit src=%u (compsrc=%u fbo1col=%u) -> FBO0 %dx%d\n",
                                      g_re4_frame,src,g_composite_src_tex,g_fbo_color_tex[1],re4_screen_width(),re4_screen_height());
  return 1;
}
/* CACHE de glGetString: o preprocessador de shader do Unity chama glGetString(RENDERER/EXTENSIONS)
   numa WORKER thread que pode NAO ter contexto GL current -> real retorna NULL -> o parse char-a-char
   de NULL crasha. Cacheamos os valores (de quando havia contexto) e devolvemos no lugar de NULL. */
static const unsigned char* g_glstr_cache[5]={0,0,0,0,0}; /* VENDOR,RENDERER,VERSION,EXT,GLSL */
static int glstr_idx(unsigned n){ switch(n){case 0x1F00:return 0;case 0x1F01:return 1;case 0x1F02:return 2;case 0x1F03:return 3;case 0x8B8C:return 4;} return -1; }
static const unsigned char* my_glGetString(unsigned n){
  if(!r_glGetString) r_glGetString=dlsym(RTLD_DEFAULT,"glGetString");
  const unsigned char* s=r_glGetString?r_glGetString(n):0;
  int i=glstr_idx(n);
  if(s){ if(i>=0 && !g_glstr_cache[i]) g_glstr_cache[i]=(const unsigned char*)strdup((const char*)s); }
  else if(i>=0 && g_glstr_cache[i]){ s=g_glstr_cache[i]; static int w=0; if(w++<8)fprintf(stderr,"[GLSTR] 0x%x NULL->cache %s\n",n,(const char*)s); }
  else if(i>=0){ /* sem cache ainda: devolve default sano (nunca NULL p/ o parser) */
    s=(const unsigned char*)(n==0x1F00?"ARM":n==0x1F01?"Mali-450 MP":n==0x1F02?"OpenGL ES 2.0":n==0x8B8C?"OpenGL ES GLSL ES 1.00":""); }
  /* GL_EXTENSIONS: o Mali-450 (Utgard) NAO amostra texturas NPOT (retorna cor solida). Se as
     extensoes anunciam NPOT, a Unity aloca render-targets NPOT (ex: 960x540) e o shader de COMPOSITE
     dela amostra essa RT -> menu achatado/escuro. Removendo os tokens NPOT, a Unity arredonda as RTs
     p/ POT (sampleaveis) -> o composite do menu volta. (RE4_KEEP_NPOT desliga o filtro.) */
  if(n==0x1F03 && s && getenv("RE4_STRIP_NPOT")){ /* opt-in: Unity ignorou p/ RTs de camera */
    static char *filtered=NULL;
    if(!filtered){
      filtered=strdup((const char*)s);
      if(filtered){
        const char *toks[]={"GL_OES_texture_npot","GL_ARB_texture_non_power_of_two",
                            "GL_APPLE_texture_2D_limited_npot","GL_IMG_texture_npot",NULL};
        for(int t=0;toks[t];t++){ char *p; size_t L=strlen(toks[t]);
          while((p=strstr(filtered,toks[t]))) for(size_t k=0;k<L;k++) p[k]=' '; }
        fprintf(stderr,"[GLSTR] EXTENSIONS filtrado (NPOT removido)\n");
      }
    }
    if(filtered) s=(const unsigned char*)filtered;
  }
	  if(getenv("RE4_GLDIAG") && (n==0x1F00||n==0x1F01||n==0x1F02||n==0x8B8C)) fprintf(stderr,"[GLSTR] 0x%x = %s\n",n,s?(const char*)s:"(null)");
  return s; }
static int re4_gl_rt_max(void){
  /* Limite p/ ENCOLHER render targets grandes demais p/ a GPU. O cap antigo era 1024
     hardcoded -> em 1280x720 a textura de cor do FBO da cena (1280x720) era dividida
     p/ 640x360, MAS o Unity continuava com glViewport(0,0,1280,720) nela -> só um canto
     renderizava -> ZOOM proporcional à resolução. Em 960x540 (<1024) nunca encolhia ->
     preenchia certo. FIX: usar o limite REAL da GPU (GL_MAX_TEXTURE_SIZE, Mali-450=4096)
     -> RTs na resolução de tela (720p/1080p/...) nunca encolhem -> sem zoom, automático
     em qualquer device. RE4_GLRT_MAX força (debug). */
  const char *v=getenv("RE4_GLRT_MAX");
  if(v&&v[0]){ char *e=NULL; long n=strtol(v,&e,10);
    if(e&&!*e){ if(n<256)n=256; if(n>8192)n=8192; return (int)n; } }
  static int cached=0;
  if(cached) return cached;
  if(!r_glGetIntegerv) r_glGetIntegerv=dlsym(RTLD_DEFAULT,"glGetIntegerv");
  if(r_glGetIntegerv){
    int mts=0, mrb=0;
    r_glGetIntegerv(0x0D33,&mts);  /* GL_MAX_TEXTURE_SIZE */
    r_glGetIntegerv(0x84E8,&mrb);  /* GL_MAX_RENDERBUFFER_SIZE */
    int m=mts; if(mrb>0 && mrb<m) m=mrb;
    if(m>=512){ cached=m; return cached; }   /* só cacheia query válida (contexto current) */
  }
  return 2048; /* fallback antes de haver contexto GL (floor seguro p/ 1080p) */
}
static int re4_next_pot(int v){ int p=1; while(p<v) p<<=1; return p; }
static int re4_bpp_for(unsigned format,unsigned type){
  if(type==0x1401){ /* GL_UNSIGNED_BYTE */
    switch(format){ case 0x1908: case 0x80E1: return 4; /* RGBA/BGRA */
                    case 0x1907: return 3;               /* RGB */
                    case 0x190A: return 2;               /* LUMINANCE_ALPHA */
                    case 0x1909: case 0x1906: return 1;  /* LUMINANCE/ALPHA */
                    default: return 0; }
  }
  if(type==0x8363||type==0x8033||type==0x8034) return 2; /* 565/4444/5551 */
  return 0;
}
/* Mali-450 amostra NPOT como cor solida -> texturas NPOT do jogo (fundo/UI do menu) saem PRETAS.
   Estica a textura NPOT pra POT (nearest) no upload; UV 0..1 segue mapeando a imagem inteira ->
   sem distorcao visivel. So p/ texturas com DADOS (pixels!=NULL) e formatos descomprimidos. */
static int re4_upload_pot(unsigned target,int level,int internalformat,int width,int height,
                          unsigned format,unsigned type,const void *pixels){
  if(!pixels || width<=0 || height<=0) return 0;
  int bpp=re4_bpp_for(format,type); if(bpp<=0) return 0;
  int pw0=re4_next_pot(width<<level), ph0=re4_next_pot(height<<level);
  int tw=pw0>>level, th=ph0>>level; if(tw<1)tw=1; if(th<1)th=1;
  if(tw==width && th==height) return 0; /* ja POT */
  unsigned char *buf=(unsigned char*)malloc((size_t)tw*th*bpp);
  if(!buf) return 0;
  const unsigned char *src=(const unsigned char*)pixels;
  for(int y=0;y<th;y++){
    int sy=(int)((long long)y*height/th); if(sy>=height)sy=height-1;
    for(int x=0;x<tw;x++){
      int sx=(int)((long long)x*width/tw); if(sx>=width)sx=width-1;
      const unsigned char *s=src+((size_t)sy*width+sx)*bpp;
      unsigned char *d=buf+((size_t)y*tw+x)*bpp;
      for(int b=0;b<bpp;b++) d[b]=s[b];
    }
  }
  r_glTexImage2D(target,level,internalformat,tw,th,0,format,type,buf);
  free(buf);
  static int lg=0; if(lg++<30) fprintf(stderr,"[NPOT->POT] L%d %dx%d -> %dx%d fmt=0x%x bpp=%d\n",level,width,height,tw,th,format,bpp);
  return 1;
}
static void my_glTexImage2D(unsigned target,int level,int internalformat,int width,int height,int border,unsigned format,unsigned type,const void *pixels){
  if(!r_glTexImage2D) r_glTexImage2D=dlsym(RTLD_DEFAULT,"glTexImage2D");
  if(r_glTexImage2D){
    int maxdim=re4_gl_rt_max();
    /* NPOT->POT p/ texturas de DADOS (Mali nao amostra NPOT) */
    if(target==0x0de1 && pixels && !getenv("RE4_NO_NPOT_POT") &&
       (re4_next_pot(width)!=width || re4_next_pot(height)!=height)){
      if(target==0x0de1){ int idx=re4_texunit_index(g_gl_active_texture);
        unsigned tex=(idx>=0)?g_gl_bound_tex2d[idx]:0; if(level==0&&tex) re4_note_texture_dims(tex,re4_next_pot(width),re4_next_pot(height)); }
      if(re4_upload_pot(target,level,internalformat,width,height,format,type,pixels)) return;
    }
    if(level==0 && pixels==NULL && (width>maxdim || height>maxdim)){
      int ow=width, oh=height; static int lg=0;
      while(width>maxdim || height>maxdim){ width=(width+1)/2; height=(height+1)/2; }
      if(lg++<24) fprintf(stderr,"[GLRT] glTexImage2D %dx%d -> %dx%d fmt=0x%x ifmt=0x%x\n",ow,oh,width,height,format,internalformat);
    }
    if(target==0x0de1){
      int idx=re4_texunit_index(g_gl_active_texture);
      unsigned tex=(idx>=0)?g_gl_bound_tex2d[idx]:0;
      if(level==0 && tex)
        re4_note_texture_dims(tex,width,height);
    }
    r_glTexImage2D(target,level,internalformat,width,height,border,format,type,pixels);
  }
}
static void my_glRenderbufferStorage(unsigned target,unsigned internalformat,int width,int height){
  if(!r_glRenderbufferStorage) r_glRenderbufferStorage=dlsym(RTLD_DEFAULT,"glRenderbufferStorage");
  if(r_glRenderbufferStorage){
    int maxdim=re4_gl_rt_max();
    if(width>maxdim || height>maxdim){
      int ow=width, oh=height; static int lg=0;
      while(width>maxdim || height>maxdim){ width=(width+1)/2; height=(height+1)/2; }
      if(lg++<24) fprintf(stderr,"[GLRT] glRenderbufferStorage %dx%d -> %dx%d ifmt=0x%x\n",ow,oh,width,height,internalformat);
    }
    r_glRenderbufferStorage(target,internalformat,width,height);
  }
}
static void my_glBindFramebuffer(unsigned target,unsigned framebuffer){
  if(!r_glBindFramebuffer) r_glBindFramebuffer=dlsym(RTLD_DEFAULT,"glBindFramebuffer");
  /* PRESENT DIFERIDO via SNAPSHOT: no inicio do proximo frame (Unity binda um FBO offscreen),
     DEPOIS do swap proprio da Unity, re-desenhamos o snapshot do composite (capturado quando o
     FBO0 era valido) no FBO0 e apresentamos. Como nosso swap e o ULTIMO, vence e chega ao fb0. */
  if(framebuffer!=0 && g_fbo0_dirty){
    g_fbo0_dirty=0;
    if(getenv("RE4_RT_CLEARTEST")){ /* diagnostico legado */
      if(r_glBindFramebuffer) r_glBindFramebuffer(0x8d40,0);
      if(!r_glClearColor) r_glClearColor=dlsym(RTLD_DEFAULT,"glClearColor");
      if(!r_glClear) r_glClear=dlsym(RTLD_DEFAULT,"glClear");
      if(r_glClearColor) r_glClearColor(1.0f,0.0f,0.0f,1.0f);
      if(r_glClear) r_glClear(0x4000);
    } else {
      re4_rt_blit_fbo1(); /* blita g_snap_tex -> FBO0 */
    }
    egl_shim_force_present("snapshot present (frame start)");
  }
  g_gl_bound_fbo=framebuffer;
  if(r_glBindFramebuffer){
    static int lg=0;
    if(lg++<32) fprintf(stderr,"[GLFB] bind target=0x%x fbo=%u\n",target,framebuffer);
    r_glBindFramebuffer(target,framebuffer);
  }
}
static unsigned my_glCheckFramebufferStatus(unsigned target){
  if(!r_glCheckFramebufferStatus) r_glCheckFramebufferStatus=dlsym(RTLD_DEFAULT,"glCheckFramebufferStatus");
  if(r_glCheckFramebufferStatus){
    unsigned status=r_glCheckFramebufferStatus(target);
    static int lg=0;
    if(lg++<32) fprintf(stderr,"[GLFB] status target=0x%x fbo=%u -> 0x%x\n",target,g_gl_bound_fbo,status);
    return status;
  }
  return 0;
}
static void my_glFramebufferTexture2D(unsigned target,unsigned attachment,unsigned textarget,unsigned texture,int level){
  if(!r_glFramebufferTexture2D) r_glFramebufferTexture2D=dlsym(RTLD_DEFAULT,"glFramebufferTexture2D");
  if(attachment==0x8ce0 && g_gl_bound_fbo < (sizeof(g_fbo_color_tex)/sizeof(g_fbo_color_tex[0])))
    g_fbo_color_tex[g_gl_bound_fbo]=texture;
  if(r_glFramebufferTexture2D){
    static int lg=0;
    if(lg++<32) fprintf(stderr,"[GLFB] tex2d fbo=%u att=0x%x tex=%u tgt=0x%x level=%d\n",g_gl_bound_fbo,attachment,texture,textarget,level);
    r_glFramebufferTexture2D(target,attachment,textarget,texture,level);
  }
}
static void my_glFramebufferRenderbuffer(unsigned target,unsigned attachment,unsigned renderbuffertarget,unsigned renderbuffer){
  if(!r_glFramebufferRenderbuffer) r_glFramebufferRenderbuffer=dlsym(RTLD_DEFAULT,"glFramebufferRenderbuffer");
  if(r_glFramebufferRenderbuffer){
    static int lg=0;
    if(lg++<32) fprintf(stderr,"[GLFB] rb fbo=%u att=0x%x rb=%u rbtarget=0x%x\n",g_gl_bound_fbo,attachment,renderbuffer,renderbuffertarget);
    r_glFramebufferRenderbuffer(target,attachment,renderbuffertarget,renderbuffer);
  }
}
static void (*r_glClearDepthf)(float);
static void (*r_glDepthMask)(unsigned char);
static void my_glClear(unsigned mask){
  if(!r_glClear) r_glClear=dlsym(RTLD_DEFAULT,"glClear");
  if(r_glClear){
    re4_count_fbo_bucket(g_gl_bound_fbo, 1);
    static int lg=0;
    if(lg++<48) fprintf(stderr,"[GLDRAW] clear fbo=%u mask=0x%x\n",g_gl_bound_fbo,mask);
    /* FIX Mali-450 (cena 3D toda navy = geometria some): no Utgard o clear de DEPTH muitas vezes
       NAO deixa o buffer em 1.0 (fica ~0) e/ou o depth-mask esta OFF -> depth-test (LESS) rejeita
       TODA a geometria 3D -> so a cor de clear aparece. Forcamos clearDepth=1.0 + depthMask ON
       antes de todo clear de profundidade (mesma raiz do NFS/Banjo). RE4_NO_DEPTHFIX desliga. */
    if((mask & 0x100) && !getenv("RE4_NO_DEPTHFIX")){
      if(!r_glClearDepthf) r_glClearDepthf=dlsym(RTLD_DEFAULT,"glClearDepthf");
      if(!r_glDepthMask) r_glDepthMask=dlsym(RTLD_DEFAULT,"glDepthMask");
      if(r_glDepthMask) r_glDepthMask(1);
      if(r_glClearDepthf) r_glClearDepthf(1.0f);
    }
    r_glClear(mask);
  }
}
/* SNAPSHOT do composite: a Unity acabou de escrever a cena no FBO0 (valido NESTE momento).
   Copiamos pra uma textura nossa (g_snap_tex) via glCopyTexImage2D, lendo o framebuffer ligado
   (FBO0). Depois, no ponto diferido (inicio do proximo frame, DEPOIS do swap proprio da Unity),
   re-desenhamos g_snap_tex no FBO0 e apresentamos -> nosso swap e o ULTIMO -> vence -> chega ao fb0.
   (Present imediato aqui NAO funciona: a Unity faz o swap real dela depois e mostra buffer preto.) */
static unsigned char *g_snap_cpu=NULL;
static void re4_snapshot_fbo0(void){
  int w=re4_screen_width(), h=re4_screen_height();
  unsigned saved_active=g_gl_active_texture;
  int saved_idx=re4_texunit_index(saved_active);
  unsigned saved_tex=(saved_idx>=0)?g_gl_bound_tex2d[saved_idx]:0;
  if(!r_glGenTextures) r_glGenTextures=dlsym(RTLD_DEFAULT,"glGenTextures");
  if(!r_glReadPixels) r_glReadPixels=dlsym(RTLD_DEFAULT,"glReadPixels");
  if(!r_glTexImage2D) r_glTexImage2D=dlsym(RTLD_DEFAULT,"glTexImage2D");
  if(!r_glBindTexture) r_glBindTexture=dlsym(RTLD_DEFAULT,"glBindTexture");
  if(!r_glActiveTexture) r_glActiveTexture=dlsym(RTLD_DEFAULT,"glActiveTexture");
  if(!r_glTexParameteri) r_glTexParameteri=dlsym(RTLD_DEFAULT,"glTexParameteri");
  if(!r_glGetError) r_glGetError=dlsym(RTLD_DEFAULT,"glGetError");
  if(!r_glGenTextures||!r_glReadPixels||!r_glTexImage2D||!r_glBindTexture||!r_glActiveTexture) return;
  /* le o FBO0 (composite valido AGORA) pra CPU */
  if(!g_snap_cpu || g_snap_w!=w || g_snap_h!=h){
    free(g_snap_cpu); g_snap_cpu=(unsigned char*)malloc((size_t)w*h*4);
  }
  if(!g_snap_cpu) return;
  if(!r_glTexSubImage2D) r_glTexSubImage2D=dlsym(RTLD_DEFAULT,"glTexSubImage2D");
  if(!r_glBindFramebuffer) r_glBindFramebuffer=dlsym(RTLD_DEFAULT,"glBindFramebuffer");
  if(r_glGetError) r_glGetError(); /* limpa */
  /* le o FBO0 (composite da Unity, valido AGORA -> a cena; PRESENTDIAG provou 113,97,80). */
  int snap_fbo=0; { const char*sv=getenv("RE4_SNAP_FBO"); if(sv&&sv[0]) snap_fbo=atoi(sv); }
  if(r_glBindFramebuffer) r_glBindFramebuffer(0x8d40,(unsigned)snap_fbo);
  r_glReadPixels(0,0,w,h,0x1908,0x1401,g_snap_cpu); /* RGBA/UBYTE */
  unsigned rerr=r_glGetError?r_glGetError():0;
  if(r_glBindFramebuffer) r_glBindFramebuffer(0x8d40,0); /* volta pro FBO0 */
  /* POT: Mali-450 Utgard amostra NPOT (960x540) como cor SOLIDA (incompleta). Aloca uma textura
     POWER-OF-TWO e poe a cena num sub-retangulo via glTexSubImage2D; a UV e escalada no blit. */
  if(r_glActiveTexture) r_glActiveTexture(0x84c0);
  if(!g_snap_tex) r_glGenTextures(1,&g_snap_tex);
  r_glBindTexture(0x0de1,g_snap_tex);
  int pot=1; while(pot<w || pot<h) pot<<=1; /* menor POT que cobre w e h */
  if(g_snap_pot!=pot){
    r_glTexImage2D(0x0de1,0,0x1908,pot,pot,0,0x1908,0x1401,NULL); /* aloca POTxPOT */
    if(r_glTexParameteri){
      r_glTexParameteri(0x0de1,0x2801,0x2601); /* MIN=LINEAR */
      r_glTexParameteri(0x0de1,0x2800,0x2601); /* MAG=LINEAR */
      r_glTexParameteri(0x0de1,0x2802,0x812f); /* WRAP_S=CLAMP */
      r_glTexParameteri(0x0de1,0x2803,0x812f); /* WRAP_T=CLAMP */
    }
    g_snap_pot=pot;
  }
  if(r_glTexSubImage2D) r_glTexSubImage2D(0x0de1,0,0,0,w,h,0x1908,0x1401,g_snap_cpu);
  unsigned terr=r_glGetError?r_glGetError():0;
  g_snap_w=w; g_snap_h=h;
  g_snap_uvw=(float)w/(float)pot; g_snap_uvh=(float)h/(float)pot;
  if(saved_idx>=0 && r_glBindTexture) r_glBindTexture(0x0de1,saved_tex);
  if(r_glActiveTexture) r_glActiveTexture(saved_active);
  unsigned char *c=g_snap_cpu+((size_t)(h/2)*w+(w/2))*4;
  static int lg=0; if(lg++<6) fprintf(stderr,"[SNAP] f=%d tex=%u %dx%d readerr=0x%x texerr=0x%x center=%u,%u,%u,%u\n",
                                      g_re4_frame,g_snap_tex,w,h,rerr,terr,c[0],c[1],c[2],c[3]);
}
/* PRESENT IMEDIATO: a Unity acabou de escrever o composite no FBO0 (window back buffer). Os dados
   provam que NESTE momento o FBO0 tem a cena (RT_DIAG f=3 FBO0=35,31,32), mas no proximo frame o
   back buffer e descartado (EGL_BUFFER_DESTROYED no Mali). Entao apresentamos AGORA, sem release no
   meio. 1x por frame: o swap destroi o back buffer; um 2o composite no mesmo frame desenharia em
   buffer indefinido -> so o 1o composite (full-screen) do frame apresenta. */
static void re4_rt_dump_fbos(void); /* fwd decl */
/* RESOLVE: forca o tile do FBO0 a ser escrito na superficie via glReadPixels FULL, ANTES do swap.
   No Mali Utgard o draw do composite fica no tile e nao e resolvido no swap -> preto. Um readpixels
   completo resolve. NAO faz blit/upload (isso corrompia o frame seguinte do Unity). */
static unsigned char *g_resolve_cpu=NULL; static int g_resolve_w=0,g_resolve_h=0;
static void re4_resolve_fbo0(void){
  int w=re4_screen_width(), h=re4_screen_height();
  if(!r_glReadPixels) r_glReadPixels=dlsym(RTLD_DEFAULT,"glReadPixels");
  if(!r_glReadPixels) return;
  if(!g_resolve_cpu || g_resolve_w!=w || g_resolve_h!=h){
    free(g_resolve_cpu); g_resolve_cpu=(unsigned char*)malloc((size_t)w*h*4);
    g_resolve_w=w; g_resolve_h=h;
  }
  if(!g_resolve_cpu) return;
  r_glReadPixels(0,0,w,h,0x1908,0x1401,g_resolve_cpu); /* le FBO0 atual (bound) = forca resolve */
}
static void re4_present_composite(void){
  /* SEM guard 1x/frame: apresentamos TODO draw no FBO0; o ULTIMO do frame (composite completo
     com texto+fundo) sobrescreve -> fb0 fica com o frame completo. (Guardar so o 1o pegava um
     composite escuro/incompleto.) RE4_PRESENT_ONCE volta ao modo 1x/frame se preciso. */
  if(getenv("RE4_PRESENT_ONCE")){
    static int last_present_frame=-1;
    if(last_present_frame==g_re4_frame) return;
    last_present_frame=g_re4_frame;
  }
  if(getenv("RE4_PRESENT_DIAG")){
    int f=g_re4_frame;
    if(f==60||f==180||f==300||f==350||f==450){
      if(!r_glReadPixels) r_glReadPixels=dlsym(RTLD_DEFAULT,"glReadPixels");
      unsigned char p[4]={0};
      if(r_glReadPixels) r_glReadPixels(re4_screen_width()/2,re4_screen_height()/2,1,1,0x1908,0x1401,p);
      fprintf(stderr,"[PRESENTDIAG] f=%d FBO0(composite)=%u,%u,%u,%u src=%u\n",f,p[0],p[1],p[2],p[3],g_composite_src_tex);
    }
  }
  re4_rt_dump_fbos(); /* diagnostico: dumpa FBOs na render thread (gated RE4_RT_DUMP_FRAME) */
  const char *mode=getenv("RE4_PRESENT_MODE");
  if(mode && !strcmp(mode,"immred")){
    if(!r_glClearColor) r_glClearColor=dlsym(RTLD_DEFAULT,"glClearColor");
    if(!r_glClear) r_glClear=dlsym(RTLD_DEFAULT,"glClear");
    if(r_glClearColor) r_glClearColor(1.0f,0.0f,0.0f,1.0f);
    if(r_glClear) r_glClear(0x4000);
  } else if(mode && !strcmp(mode,"immblit")){
    re4_rt_blit_fbo1();    /* blita g_composite_src_tex (tex1) direto */
  } else if(mode && !strcmp(mode,"immsnap")){
    /* snapshot FBO0 -> POT g_snap_tex -> blit (present pipeline validado; mostra o composite real).
       Hoje o composite mid-frame esta plano, entao isto mostra cinza-escuro; deixado p/ continuacao. */
    re4_snapshot_fbo0();
    re4_rt_blit_fbo1();
  }
  else {
    /* DEFAULT: present direto. O resolve (readpixels full/frame) e LENTO e desnecessario com o
       deadlock quebrado -> opt-in via RE4_RESOLVE. */
    if(getenv("RE4_RESOLVE")) re4_resolve_fbo0();
  }
  egl_shim_force_present("composite-draw");
}
/* DIAGNOSTICO render-thread: no frame alvo, dumpa FBO0/FBO1/FBO2 LENDO NA RENDER THREAD (contexto
   certo; o dump do main loop le FBO invalido). Mostra QUAL fbo tem a cena com detalhe e quando. */
static void re4_rt_dump_fbos(void){
  static int shots=0; static int nextf=-1;
  const char *fv=getenv("RE4_RT_DUMP_FRAME");
  int tf=(fv&&fv[0])?atoi(fv):-1;
  if(tf<0 || shots>=6 || g_re4_frame<tf) return;
  if(nextf<0) nextf=tf;
  if(g_re4_frame<nextf) return;
  nextf=g_re4_frame+25; shots++;  /* multi-shot: a cada ~25 frames, 6x */
  if(!r_glReadPixels) r_glReadPixels=dlsym(RTLD_DEFAULT,"glReadPixels");
  if(!r_glBindFramebuffer) r_glBindFramebuffer=dlsym(RTLD_DEFAULT,"glBindFramebuffer");
  if(!r_glReadPixels||!r_glBindFramebuffer) return;
  int w=re4_screen_width(), h=re4_screen_height();
  unsigned char *buf=(unsigned char*)malloc((size_t)w*h*4);
  if(!buf) return;
  unsigned saved=g_gl_bound_fbo;
  int fbos[4]={0,1,2,saved}; const char* names[4]={"rt-fbo0","rt-fbo1","rt-fbo2","rt-cur"};
  for(int i=0;i<4;i++){
    if(i==3 && (saved==0||saved==1||saved==2)) continue;
    r_glBindFramebuffer(0x8d40,(unsigned)fbos[i]);
    memset(buf,0,(size_t)w*h*4);
    r_glReadPixels(0,0,w,h,0x1908,0x1401,buf);
    /* variancia rapida (canal R) */
    int mn=255,mx=0; long sum=0; int n=0;
    for(int p=0;p<w*h;p+=37){ int v=buf[p*4]; if(v<mn)mn=v; if(v>mx)mx=v; sum+=v; n++; }
    fprintf(stderr,"[RTDUMP] f=%d %s fbo=%d Rmin=%d Rmax=%d Rmean=%ld span=%d\n",
            g_re4_frame,names[i],fbos[i],mn,mx,n?sum/n:0,mx-mn);
    re4_dump_rgba_file(names[i],g_re4_frame,w,h,buf);
  }
  r_glBindFramebuffer(0x8d40,saved);
  free(buf);
}
/* Chamado pelo egl_shim quando a RENDER THREAD solta a window (fim do frame): o FBO0 tem o frame
   FINAL do Unity. snapshot+blit+present captura a cena renderizada por ultimo. */
void re4_frame_end_present(void){
  if(!getenv("RE4_FRAMEEND")) return;
  re4_snapshot_fbo0();   /* readpixels FBO0 (window, frame final) -> g_snap_tex POT */
  re4_rt_blit_fbo1();    /* blita g_snap_tex -> FBO0 */
  egl_shim_force_present("frame-end-release");
}
/* No draw para o FBO0 (composite final da Unity), captura a textura que a Unity esta amostrando
   (a cena/menu). Pega a maior textura conhecida entre as unidades ligadas; fallback unidade 0. */
static void re4_capture_composite_src(const char *kind, int count){
  unsigned best=0; int best_area=-1;
  for(int u=0;u<4;u++){
    unsigned t=g_gl_bound_tex2d[u];
    if(!t) continue;
    int w=0,h=0; int area=re4_get_texture_dims(t,&w,&h)?(w*h):0;
    if(area>best_area){ best_area=area; best=t; }
  }
  if(!best) best=g_gl_bound_tex2d[0];
  static int lg=0;
  if(lg++<40)
    fprintf(stderr,"[COMPSRC] f=%d via=%s count=%d prog=%u u0=%u u1=%u u2=%u u3=%u -> src=%u area=%d\n",
            g_re4_frame,kind?kind:"?",count,g_gl_current_program,
            g_gl_bound_tex2d[0],g_gl_bound_tex2d[1],g_gl_bound_tex2d[2],g_gl_bound_tex2d[3],best,best_area);
  if(best) g_composite_src_tex=best;
}
static void my_glDrawArrays(unsigned mode,int first,int count){
  int saved_scissor_enabled=0;
  int saved_scissor[4]={0,0,0,0};
  int fix_scissor=0;
  if(!r_glDrawArrays) r_glDrawArrays=dlsym(RTLD_DEFAULT,"glDrawArrays");
  if(r_glDrawArrays){
    re4_count_fbo_bucket(g_gl_bound_fbo, 0);
    re4_log_composite_state("arrays", mode, count); re4_log_scene_draw("arrays", mode, count);
    fix_scissor=re4_begin_composite_scissor_override("arrays",count,&saved_scissor_enabled,saved_scissor);
    static int lg=0;
    if(lg++<64) fprintf(stderr,"[GLDRAW] arrays fbo=%u mode=0x%x first=%d count=%d\n",g_gl_bound_fbo,mode,first,count);
    r_glDrawArrays(mode,first,count);
    re4_dump_window_draw_if_needed("win-arrays");
    re4_end_composite_scissor_override(fix_scissor,saved_scissor_enabled,saved_scissor);
    if(!g_gl_bound_fbo){ const char*_m=getenv("RE4_PRESENT_MODE"); re4_capture_composite_src("arrays",count); if(_m&&!strcmp(_m,"snap")){re4_snapshot_fbo0();g_fbo0_dirty=1;} else re4_present_composite(); }
  }
}
static void my_glDrawElements(unsigned mode,int count,unsigned type,const void *indices){
  int saved_scissor_enabled=0;
  int saved_scissor[4]={0,0,0,0};
  int fix_scissor=0;
  if(!r_glDrawElements) r_glDrawElements=dlsym(RTLD_DEFAULT,"glDrawElements");
  if(r_glDrawElements){
    re4_count_fbo_bucket(g_gl_bound_fbo, 0);
    re4_log_composite_state("elems", mode, count); re4_log_scene_draw("elems", mode, count);
    fix_scissor=re4_begin_composite_scissor_override("elems",count,&saved_scissor_enabled,saved_scissor);
    static int lg=0;
    if(lg++<64) fprintf(stderr,"[GLDRAW] elems fbo=%u mode=0x%x count=%d type=0x%x idx=%p\n",g_gl_bound_fbo,mode,count,type,indices);
    r_glDrawElements(mode,count,type,indices);
    re4_dump_window_draw_if_needed("win-elems");
    re4_end_composite_scissor_override(fix_scissor,saved_scissor_enabled,saved_scissor);
    if(!g_gl_bound_fbo){ const char*_m=getenv("RE4_PRESENT_MODE"); re4_capture_composite_src("elems",count); if(_m&&!strcmp(_m,"snap")){re4_snapshot_fbo0();g_fbo0_dirty=1;} else re4_present_composite(); }
  }
}
static void my_glUseProgram(unsigned program){
  if(!r_glUseProgram) r_glUseProgram=dlsym(RTLD_DEFAULT,"glUseProgram");
  g_gl_current_program=program;
  if(r_glUseProgram) r_glUseProgram(program);
}
static void my_glActiveTexture(unsigned texture){
  if(!r_glActiveTexture) r_glActiveTexture=dlsym(RTLD_DEFAULT,"glActiveTexture");
  g_gl_active_texture=texture;
  if(r_glActiveTexture) r_glActiveTexture(texture);
}
static void my_glBindTexture(unsigned target,unsigned texture){
  if(!r_glBindTexture) r_glBindTexture=dlsym(RTLD_DEFAULT,"glBindTexture");
  int idx=re4_texunit_index(g_gl_active_texture);
  if(idx>=0){
    if(target==0x0de1) g_gl_bound_tex2d[idx]=texture;      /* GL_TEXTURE_2D */
    else if(target==0x8d65) g_gl_bound_texext[idx]=texture; /* GL_TEXTURE_EXTERNAL_OES */
  }
  if(r_glBindTexture) r_glBindTexture(target,texture);
}
static void my_glViewport(int x,int y,int width,int height){
  if(!r_glViewport) r_glViewport=dlsym(RTLD_DEFAULT,"glViewport");
  g_gl_viewport[0]=x; g_gl_viewport[1]=y; g_gl_viewport[2]=width; g_gl_viewport[3]=height;
  { static int n=0; if(getenv("RE4_VPLOG") && n++<60)
      fprintf(stderr,"[VPLOG] glViewport(%d,%d,%d,%d) fbo=%u screen=%dx%d\n",
              x,y,width,height,g_gl_bound_fbo,re4_screen_width(),re4_screen_height()); }
  if(r_glViewport) r_glViewport(x,y,width,height);
}
static void my_glScissor(int x,int y,int width,int height){
  if(!r_glScissor) r_glScissor=dlsym(RTLD_DEFAULT,"glScissor");
  g_gl_scissor[0]=x; g_gl_scissor[1]=y; g_gl_scissor[2]=width; g_gl_scissor[3]=height;
  if(r_glScissor) r_glScissor(x,y,width,height);
}
static void my_glEnable(unsigned cap){
  if(!r_glEnable) r_glEnable=dlsym(RTLD_DEFAULT,"glEnable");
  if(cap==0x0be2) g_gl_blend_enabled=1; /* GL_BLEND */
  if(cap==0x0c11) g_gl_scissor_enabled=1; /* GL_SCISSOR_TEST */
  if(r_glEnable) r_glEnable(cap);
}
static void my_glDisable(unsigned cap){
  if(!r_glDisable) r_glDisable=dlsym(RTLD_DEFAULT,"glDisable");
  if(cap==0x0be2) g_gl_blend_enabled=0; /* GL_BLEND */
  if(cap==0x0c11) g_gl_scissor_enabled=0; /* GL_SCISSOR_TEST */
  if(r_glDisable) r_glDisable(cap);
}
static void my_glBlendFunc(unsigned sfactor,unsigned dfactor){
  if(!r_glBlendFunc) r_glBlendFunc=dlsym(RTLD_DEFAULT,"glBlendFunc");
  g_gl_blend_src_rgb=sfactor; g_gl_blend_dst_rgb=dfactor;
  g_gl_blend_src_alpha=sfactor; g_gl_blend_dst_alpha=dfactor;
  if(r_glBlendFunc) r_glBlendFunc(sfactor,dfactor);
}
static void my_glBlendFuncSeparate(unsigned srcRGB,unsigned dstRGB,unsigned srcAlpha,unsigned dstAlpha){
  if(!r_glBlendFuncSeparate) r_glBlendFuncSeparate=dlsym(RTLD_DEFAULT,"glBlendFuncSeparate");
  g_gl_blend_src_rgb=srcRGB; g_gl_blend_dst_rgb=dstRGB;
  g_gl_blend_src_alpha=srcAlpha; g_gl_blend_dst_alpha=dstAlpha;
  if(r_glBlendFuncSeparate) r_glBlendFuncSeparate(srcRGB,dstRGB,srcAlpha,dstAlpha);
}
static void my_glColorMask(unsigned char red,unsigned char green,unsigned char blue,unsigned char alpha){
  if(!r_glColorMask) r_glColorMask=dlsym(RTLD_DEFAULT,"glColorMask");
  g_gl_color_mask[0]=red; g_gl_color_mask[1]=green; g_gl_color_mask[2]=blue; g_gl_color_mask[3]=alpha;
  if(r_glColorMask) r_glColorMask(red,green,blue,alpha);
}
static void my_glShaderSource(unsigned sh,int c,const char*const*str,const int*len){
  if(!r_glShaderSource) r_glShaderSource=dlsym(RTLD_DEFAULT,"glShaderSource");
  if(str&&c>0&&str[0]){ char b[90]; int k=0; for(;k<89&&str[0][k]&&str[0][k]!='\n';k++)b[k]=str[0][k]; b[k]=0;
    static int n=0; if(n++<12)fprintf(stderr,"[SHADER src#%d] %s\n",n,b); }
  if(r_glShaderSource) r_glShaderSource(sh,c,str,len); }
static void my_glCompileShader(unsigned sh){
  if(!r_glCompileShader){ r_glCompileShader=dlsym(RTLD_DEFAULT,"glCompileShader"); r_glGetShaderiv=dlsym(RTLD_DEFAULT,"glGetShaderiv"); r_glGetShaderInfoLog=dlsym(RTLD_DEFAULT,"glGetShaderInfoLog"); }
  if(r_glCompileShader) r_glCompileShader(sh);
  if(r_glGetShaderiv){ int ok=0; r_glGetShaderiv(sh,0x8B81,&ok);
    static int n=0; if(!ok && n++<8){ char lg[300]; lg[0]=0; if(r_glGetShaderInfoLog)r_glGetShaderInfoLog(sh,299,0,lg); fprintf(stderr,"[SHADER FAIL] %s\n",lg); } } }
static int my_raise(int sig); static void my_abort(void); static int my_ptkill(unsigned long t,int sig);
void *re4_gl_override(const char *nm){
  if(!nm) return NULL;
  if(!strcmp(nm,"glGetString")) return (void*)my_glGetString;
  if(!strcmp(nm,"glBindFramebuffer")) return (void*)my_glBindFramebuffer;
  if(!strcmp(nm,"glCheckFramebufferStatus")) return (void*)my_glCheckFramebufferStatus;
  if(!strcmp(nm,"glFramebufferTexture2D")) return (void*)my_glFramebufferTexture2D;
  if(!strcmp(nm,"glFramebufferRenderbuffer")) return (void*)my_glFramebufferRenderbuffer;
  if(!strcmp(nm,"glClear")) return (void*)my_glClear;
  if(!strcmp(nm,"glDrawArrays")) return (void*)my_glDrawArrays;
  if(!strcmp(nm,"glDrawElements")) return (void*)my_glDrawElements;
  if(!strcmp(nm,"glUseProgram")) return (void*)my_glUseProgram;
  if(!strcmp(nm,"glActiveTexture")) return (void*)my_glActiveTexture;
  if(!strcmp(nm,"glBindTexture")) return (void*)my_glBindTexture;
  if(!strcmp(nm,"glViewport")) return (void*)my_glViewport;
  if(!strcmp(nm,"glScissor")) return (void*)my_glScissor;
  if(!strcmp(nm,"glEnable")) return (void*)my_glEnable;
  if(!strcmp(nm,"glDisable")) return (void*)my_glDisable;
  if(!strcmp(nm,"glBlendFunc")) return (void*)my_glBlendFunc;
  if(!strcmp(nm,"glBlendFuncSeparate")) return (void*)my_glBlendFuncSeparate;
  if(!strcmp(nm,"glColorMask")) return (void*)my_glColorMask;
  if(!strcmp(nm,"glTexImage2D")) return (void*)my_glTexImage2D;
  if(!strcmp(nm,"glRenderbufferStorage")) return (void*)my_glRenderbufferStorage;
  if(getenv("RE4_GLDIAG")){
    if(!strcmp(nm,"glShaderSource")) return (void*)my_glShaderSource;
    if(!strcmp(nm,"glCompileShader")) return (void*)my_glCompileShader;
  }
  return NULL;
}
static void *my_dlsym(void *h,const char *nm){ void *p=0;
  fprintf(stderr,"[DLSYM?] %s\n",nm?nm:"?"); fflush(stderr);
  /* libmono pega pthread_kill/raise/abort via dlsym(RTLD_DEFAULT), furando o GOT override.
     Devolve nossos hooks aqui tb -> caimos no map_caller (acha quem dispara o raise fatal). */
  if(nm){ if(!strcmp(nm,"pthread_kill"))return (void*)my_ptkill; if(!strcmp(nm,"raise")||!strcmp(nm,"gsignal"))return (void*)my_raise; if(!strcmp(nm,"abort"))return (void*)my_abort; }
  /* AUDIO: dlsym do handle de libOpenSLES -> opensles_shim (slCreateEngine + SL_IID_*) */
  if(h==&g_dl_sl && nm){
    if(!strcmp(nm,"slCreateEngine")) return (void*)slCreateEngine_shim;
    if(!strcmp(nm,"SL_IID_ENGINE")) return (void*)&sl_IID_ENGINE;
    if(!strcmp(nm,"SL_IID_PLAY")) return (void*)&sl_IID_PLAY;
    if(!strcmp(nm,"SL_IID_VOLUME")) return (void*)&sl_IID_VOLUME;
    if(!strcmp(nm,"SL_IID_BUFFERQUEUE")||!strcmp(nm,"SL_IID_ANDROIDSIMPLEBUFFERQUEUE")) return (void*)&sl_IID_BUFFERQUEUE;
    if(!strcmp(nm,"SL_IID_EFFECTSEND")) return (void*)&sl_IID_EFFECTSEND;
    if(!strcmp(nm,"SL_IID_ENGINECAPABILITIES")) return (void*)&sl_IID_ENGINECAPABILITIES;
    if(!strcmp(nm,"SL_IID_ENVIRONMENTALREVERB")) return (void*)&sl_IID_ENVIRONMENTALREVERB;
    fprintf(stderr,"[SL] dlsym %s -> NULL\n",nm); return NULL; }
  if(nm){
    if(!strcmp(nm,"XInputGamePadGetState")){
      fprintf(stderr,"[DLSYM-XINPUT] %s -> SDL_GameController\n",nm);
      return (void*)my_xinput_get_state;
    }
    if(!strcmp(nm,"XInputGamePadSetState")){
      fprintf(stderr,"[DLSYM-XINPUT] %s -> no-op\n",nm);
      return (void*)my_xinput_set_state;
    }
  }
  p=re4_gl_override(nm);
  if(p) return p;
  /* CRITICO: Unity dlopen libGLESv2.so/libEGL + dlsym("eglCreateContext"/etc) em runtime.
     Sem isso, cai no libEGL REAL do Mali (dep do SDL2) com nosso display FALSO -> a validacao
     de config (eglCreateContext por config) falha -> "Unable to find a configuration matching
     minimum spec" -> abort. Devolve os egl_shim_* da nossa tabela p/ TODO nome egl*. */
  if(nm && nm[0]=='e' && nm[1]=='g' && nm[2]=='l'){
    for(size_t i=0;i<dynlib_numfunctions;i++) if(!strcmp(dynlib_functions[i].symbol,nm) && dynlib_functions[i].func){
      fprintf(stderr,"[DLSYM-EGL] %s -> egl_shim\n",nm); return (void*)dynlib_functions[i].func; } }
  if(h==&g_dl_self){ p=(void*)so_find_addr_safe(nm);
    if(!p && g_m_mono){ so_module *cur=so_save(); so_use(g_m_mono); p=(void*)so_find_addr_safe(nm); so_use(cur); free(cur); }
    if(!p) p=dlsym(RTLD_DEFAULT,nm); }
  else p=dlsym(h,nm);
  fprintf(stderr,"[DLSYM=] %s -> %p\n",nm?nm:"?",p); return p; }
/* getpwuid/getpwnam do glibc fazem dlopen de NSS -> crasha no so-loader. Stub fake. */
static struct passwd g_pw;
static struct passwd *my_getpwuid(unsigned u){ (void)u; g_pw.pw_name=(char*)"user"; g_pw.pw_passwd=(char*)"";
  g_pw.pw_uid=0; g_pw.pw_gid=0; g_pw.pw_gecos=(char*)""; g_pw.pw_dir=(char*)re4_gamedir(); g_pw.pw_shell=(char*)"/bin/sh";
  fprintf(stderr,"[PWUID] stub\n"); return &g_pw; }
static const char *my_dlerror(void){ return 0; } /* sem erro -> evita _dl_exception_create */
static int my_dladdr(const void *a,void *info){ (void)a;(void)info; return 0; }
static int my_dlclose(void *h){ (void)h; return 0; }
/* hooks pra VER a excecao que o Mono lanca (antes do throw crashar) */
static void hook_exc_msg(void *img,const char *ns,const char *name,const char *msg){ (void)img;
  fprintf(stderr,"\n*** [MONO-EXC] %s.%s : %s ***\n",ns?ns:"?",name?name:"?",msg?msg:"(sem msg)"); fflush(stderr); _exit(42); }
static void map_caller(const char*tag,unsigned long ra);
static void hook_exc_two(void *img,const char *ns,const char *name,const char *m1,const char *m2){ (void)img;(void)m2;
  fprintf(stderr,"\n*** [MONO-EXC2] %s.%s : %s ***\n",ns?ns:"?",name?name:"?",m1?m1:"?");
  map_caller("[MONO-EXC2]",(unsigned long)__builtin_return_address(0));
  if(getenv("RE4_EXC_CONTINUE")){ fprintf(stderr,"[MONO-EXC2] continuando (gated)\n"); fflush(stderr); return; }
  fflush(stderr); _exit(42); }
static void hook_exc_name(void *img,const char *ns,const char *name){ (void)img;
  fprintf(stderr,"\n*** [MONO-EXC-N] %s.%s ***\n",ns?ns:"?",name?name:"?"); fflush(stderr); _exit(42); }
/* loga mmap/mprotect EXEC + falhas -> ve a alocacao de exec-mem do JIT que da NULL */
static void *my_mmap(void *a,size_t l,int prot,int flags,int fd,long off){
  if(l>1024UL*1024*1024){ fprintf(stderr,"[MMAP-BIG] %zu valloc=%p GC-caller=%p\n",l,__builtin_return_address(0),__builtin_return_address(1)); }
  /* FS exFAT/FAT do /storage/roms NAO suporta mmap de arquivo de verdade (mmap "passa" mas
     devolve lixo). Pra QUALQUER mmap de arquivo private read-only, emulo: anon RW + pread.
     Sem isso o Mono parseia lixo -> "invalid CIL image". */
  if(fd>=0 && l>0 && !(flags&0x10/*MAP_FIXED*/) && (prot&PROT_READ) && !(prot&PROT_WRITE)){
    void *q=mmap(0,l,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(q!=MAP_FAILED){ ssize_t r=pread(fd,q,l,off);
      static int e=0; if(e++<12) fprintf(stderr,"[MMAP-FILE-EMU] len=%zu fd=%d off=%ld read=%zd -> %p\n",l,fd,off,r,q);
      if(r>0) return q; munmap(q,l); }
  }
  void *p=mmap(a,l,prot,flags,fd,off);
  if(p==MAP_FAILED && fd>=0 && l>0){
    void *q=mmap(0,l,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(q!=MAP_FAILED){ ssize_t r=pread(fd,q,l,off); if(r>0) return q; munmap(q,l); }
  }
  if((prot&PROT_EXEC)||p==MAP_FAILED){ static int n=0; if(n++<60) fprintf(stderr,"[MMAP] len=%zu prot=0x%x fd=%d -> %p\n",l,prot,fd,p==MAP_FAILED?(void*)-1:p); } return p; }
static int my_mprotect(void *a,size_t l,int prot){ int r=mprotect(a,l,prot);
  if(prot&PROT_EXEC){ static int n=0; if(n++<60) fprintf(stderr,"[MPROT-X] %p len=%zu prot=0x%x -> %d(%s)\n",a,l,prot,r,r?strerror(errno):"ok"); } return r; }
/* RAIZ do OutOfMemoryException + GC_page_size==0: libmono (BIONIC) chama sysconf com as
   constantes _SC_* do BIONIC, que NAO batem com as do glibc. Ex: sysconf(40)=_SC_PAGESIZE bionic,
   mas glibc 40 = outra coisa -> page size lixo -> GC_page_size errado -> heap=0 -> OOM.
   Traducao bionic->valor correto (constantes de bionic/libc/include/bits/sysconf.h). */
/* nro de CPUs reportado ao Unity -> dimensiona o pool de job workers. Default 2 (1 worker).
   RE4_NPROC ajusta p/ testar starvation do pool (workers travam em sub-jobs sem worker livre). */
int re4_nproc(void){ const char *e=getenv("RE4_NPROC"); int n=(e&&e[0])?atoi(e):2; if(n<1)n=1; if(n>16)n=16; return n; }
static long my_sysconf(int name){
  long ps=4096;
  switch(name){
    case 39: case 40: /* bionic _SC_PAGE_SIZE / _SC_PAGESIZE */
      fprintf(stderr,"[SYSCONF] bionic PAGESIZE(%d) -> 4096\n",name); return 4096;
    case 6:  /* bionic _SC_CLK_TCK */ return 100;
    case 96: case 97: /* bionic _SC_NPROCESSORS_CONF / _ONLN */
      /* N CPUs -> Unity cria N-1 job workers. Com 1 core dava 0 workers e o WaitForJobGroup
         travava (ninguem roda inline). RE4_NPROC ajusta (testar starvation). */
      { long n=re4_nproc(); fprintf(stderr,"[SYSCONF] bionic NPROC(%d) -> %ld\n",name,n); return n; }
    case 98: /* bionic _SC_PHYS_PAGES */
      fprintf(stderr,"[SYSCONF] bionic PHYS_PAGES -> 512MB\n"); return (512L*1024*1024)/ps;
    case 99: /* bionic _SC_AVPHYS_PAGES */
      fprintf(stderr,"[SYSCONF] bionic AVPHYS_PAGES -> 256MB\n"); return (256L*1024*1024)/ps;
    default: break;
  }
  long r=sysconf(name);
  if((name==_SC_PHYS_PAGES||name==_SC_AVPHYS_PAGES) && r<=0){ r=(512L*1024*1024)/ps; }
  static int sc=0; if(sc++<30) fprintf(stderr,"[SYSCONF] glibc name=%d -> %ld\n",name,r);
  return r; }
/* BUG ACHADO: mono_pagesize() retorna 3.8GB (lixo) em vez de 4096 -> sgen pede mmap gigante.
   Forco 4096 (valor correto) -> conserta TODOS os tamanhos/alinhamentos do Mono. */
static int my_mono_pagesize(void){ return 4096; }
/* valloc clamp como rede de seguranca (caso algum tamanho absurdo escape) */
static void *my_mono_valloc(void *addr,size_t size,int flags,int type){ (void)type;(void)addr;
  if(size>256UL*1024*1024){ fprintf(stderr,"[VALLOC-CLAMP] %zu -> 256MB\n",size); size=256UL*1024*1024; }
  int prot=0; if(flags&1)prot|=PROT_READ; if(flags&2)prot|=PROT_WRITE; if(flags&4)prot|=PROT_EXEC;
  void *p=mmap(0,size,prot?prot:PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0); return p==MAP_FAILED?0:p; }
/* getter do jit_tls (libmono+0x1a6a8) asserta se a thread nao foi atachada ao Mono.
   Hook + trampolim: atacha a thread (1x) e roda o getter original. */
static void* (*g_orig_jitgetter)(void)=0;
static void* (*g_grd_fn)(void)=0; static void* (*g_dget_fn)(void)=0; static void (*g_dset_fn)(void*,int)=0; static void (*g_jatt_fn)(void*)=0;
static __thread int g_jit_attached=0;
static void* my_jit_tls_getter(void){
  static int ent=0; if(ent++<6){ fprintf(stderr,"[JITTLS] getter chamado #%d attached=%d tid=%p\n",ent,g_jit_attached,(void*)pthread_self()); fflush(stderr); }
  if(!g_jit_attached){ g_jit_attached=1;
    void* d = g_grd_fn?g_grd_fn():0;
    if(!d && g_dget_fn) d=g_dget_fn();
    if(d && g_dset_fn) g_dset_fn(d,0);
    if(d && g_jatt_fn){ g_jatt_fn(d); fprintf(stderr,"[JITTLS] thread atachada d=%p tid=%p\n",d,(void*)pthread_self()); fflush(stderr); }
    else { fprintf(stderr,"[JITTLS] sem attach (d=%p jatt=%p)\n",d,(void*)g_jatt_fn); fflush(stderr); }
  }
  return g_orig_jitgetter?g_orig_jitgetter():0;
}
/* GET_MEM do Boehm (libmono+0x2bed14): aborta "Bad GET_MEM arg" se (bytes & (GC_page_size-1)).
   Se GC_page_size==0 -> qualquer bytes aborta. Hook+trampolim: arredonda bytes p/ 4096. */
static void* (*g_orig_getmem)(unsigned)=0;
static void* my_getmem(unsigned bytes){
  /* BYPASS: se GC_page_size==0, o check do original (bytes&(page-1)) sempre aborta.
     Aloco eu mesmo (mmap page-aligned, zerado) = o que o GET_MEM do Boehm deve devolver. */
  static int gn=0; unsigned orig=bytes; bytes=(bytes+4095u)&~4095u; if(!bytes)bytes=4096;
  void *p=mmap(0,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0); if(p==MAP_FAILED)p=0;
  if(gn++<12){ fprintf(stderr,"[GETMEM] bytes=%u->%u -> %p (bypass mmap)\n",orig,bytes,p); fflush(stderr); }
  (void)g_orig_getmem; return p;
}
/* mono_jit_init_version (0xbd5d0): Unity passa versao que o Mono nao reconhece -> default v1.1
   -> nao le o mscorlib v2.0 ("invalid CIL image"). Forcamos "v2.0.50727". */
static void* (*g_orig_jitinitver)(const char*,const char*)=0;
static void* my_jit_init_version(const char* name, const char* ver){
  fprintf(stderr,"[JITINIT] name=%s ver=%s -> forco v2.0.50727\n", name?name:"?", ver?ver:"NULL"); fflush(stderr);
  return g_orig_jitinitver?g_orig_jitinitver(name,"v2.0.50727"):0;
}
/* handler de assert do Mono (libmono+0x2bcf90): r0=arquivo r1=linha. Loga e NAO aborta. */
static void my_assert_handler(const char* file, int line, const char* a, const char* b){ (void)a;(void)b;
  static int n=0; if(n++<60){ fprintf(stderr,"[ASSERT-SKIP] %s:%d\n", file?file:"?", line); fflush(stderr); }
}
/* RE4 (port de Windows/Android) monta paths de SAVE com '\' (separador Windows):
   "/roms/ports/re4/userdata\quickSave6.json". No Linux '\' e literal -> arquivo nao encontrado
   -> IOException no load do quicksave -> gameplay nao entra (tela azul presa). Normaliza '\'->'/'.
   Retorna o proprio ponteiro se nao ha '\' (sem custo); senao copia p/ buffer thread-local. */
static const char *re4_fixpath(const char *p){
  if(!p || !strchr(p,'\\')) return p;
  static __thread char buf[1024];
  size_t i=0; for(; p[i] && i<sizeof(buf)-1; i++) buf[i] = (p[i]=='\\') ? '/' : p[i];
  buf[i]=0;
  static int n=0; if(n++<20) fprintf(stderr,"[PATHFIX] '%s' -> '%s'\n",p,buf);
  return buf;
}
/* loga open/fopen -> acha a fonte de memoria (/proc/meminfo etc) */
static FILE* my_fopen(const char*p0,const char*m){ const char*p=re4_fixpath(p0); if(p&&(strstr(p,"proc")||strstr(p,"mem")||strstr(p,"sys"))) fprintf(stderr,"[FOPEN] %s\n",p);
  if(p&&!strcmp(p,"/proc/meminfo")){ fprintf(stderr,"[FOPEN] meminfo -> fake 512MB\n");
    FILE*t=tmpfile(); if(t){ fputs("MemTotal:      524288 kB\nMemFree:       262144 kB\nMemAvailable:  262144 kB\n",t); rewind(t); return t; } }
  /* core count vem daqui (/sys/.../possible|present) -> Unity dimensiona job workers. Forcamos 1
     core ("0") -> jobs INLINE, sem workers, sem WaitForJobGroup deadlock. */
  if(p&&(!strcmp(p,"/sys/devices/system/cpu/possible")||!strcmp(p,"/sys/devices/system/cpu/present")||!strcmp(p,"/sys/devices/system/cpu/online"))){
    int nc=re4_nproc(); fprintf(stderr,"[FOPEN] %s -> fake %d cores\n",p,nc); FILE*t=tmpfile(); if(t){ if(nc<=1)fputs("0\n",t); else fprintf(t,"0-%d\n",nc-1); rewind(t); return t; } }
  return fopen(p,m); }
static int my_open(const char*p0,int fl,...){ const char*p=re4_fixpath(p0); if(p&&(strstr(p,"proc")||strstr(p,"mem"))) fprintf(stderr,"[OPEN] %s\n",p);
  /* /proc/cpuinfo: Unity conta cores p/ dimensionar o job worker pool. Forcamos 1 core (1 entrada
     "processor") -> jobs rodam INLINE, sem workers, sem WaitForJobGroup deadlock. */
  if(p&&!strcmp(p,"/proc/cpuinfo")){ FILE*t=tmpfile(); int nc=re4_nproc();
    if(t){ for(int i=0;i<nc;i++) fprintf(t,"processor\t: %d\nmodel name\t: ARMv7 Processor rev 1 (v7l)\nFeatures\t: half thumb fastmult vfp edsp neon vfpv3\nCPU implementer\t: 0x41\nCPU architecture: 7\n\n",i); fflush(t); int fd=dup(fileno(t)); fclose(t); lseek(fd,0,SEEK_SET); fprintf(stderr,"[OPEN] cpuinfo -> fake %d cores (fd=%d)\n",nc,fd); return fd; } }
  if(p&&(!strcmp(p,"/sys/devices/system/cpu/possible")||!strcmp(p,"/sys/devices/system/cpu/present")||!strcmp(p,"/sys/devices/system/cpu/online"))){
    FILE*t=tmpfile(); int nc=re4_nproc(); if(t){ if(nc<=1)fputs("0\n",t); else fprintf(t,"0-%d\n",nc-1); fflush(t); int fd=dup(fileno(t)); fclose(t); lseek(fd,0,SEEK_SET); fprintf(stderr,"[OPEN] %s -> fake %d cores\n",p,nc); return fd; } }
  va_list ap; va_start(ap,fl); int mo=va_arg(ap,int); va_end(ap); return open(p,fl,mo); }
/* ANativeWindow: a Unity (nativeRecreateGfxState) chama ANativeWindow_fromSurface(env,surface)
   e ESPERA num cond ate o global de window virar !=NULL. Estavam STUBADOS (retornavam NULL)
   -> Unity guardava NULL -> UnityMain travava p/ sempre. Retornamos window fake !=NULL + dims. */
static int jobwait_stub(void*this_){ (void)this_; return 0; }
/* __system_property_get(name,value): Unity le `value` como string. O stub_generic nao
   null-terminava -> Unity lia lixo -> crash em strchrnul/strlen. Aqui zeramos value. */
static int my_sysprop(const char*name,char*value){ (void)name; if(value)value[0]=0; return 0; }
static int g_anw=0xA11;
static int re4_int_env(const char *name, int fallback, int min_value, int max_value){
  const char *value=getenv(name); char *end=NULL; long parsed;
  if(!value||!value[0]) return fallback;
  parsed=strtol(value,&end,10);
  if(!end||*end) return fallback;
  if(parsed<min_value) parsed=min_value;
  if(parsed>max_value) parsed=max_value;
  return (int)parsed;
}
static int re4_screen_width(void){ return re4_int_env("RE4_WIDTH",1280,320,1920); }
static int re4_screen_height(void){ return re4_int_env("RE4_HEIGHT",720,240,1080); }
static void *my_aw_fromSurface(void*env,void*surf){ (void)env;(void)surf; fprintf(stderr,"[ANW] fromSurface -> %p\n",(void*)&g_anw); return &g_anw; }
static int my_aw_setgeom(void*w,int wd,int ht,int f){ (void)w;(void)wd;(void)ht;(void)f; return 0; }
static int my_aw_getWidth(void*w){ (void)w; return re4_screen_width(); }
static int my_aw_getHeight(void*w){ (void)w; return re4_screen_height(); }
static void my_aw_acquire(void*w){ (void)w; }
static void my_aw_release(void*w){ (void)w; }
static const char *re4_gamedir(void){
  const char *dir=getenv("RE4_GAMEDIR");
  return (dir&&dir[0])?dir:"/storage/roms/ports/re4";
}
extern void *text_virtbase;
extern void re4_fill(void);
extern void recon_wire_pthread(void (*)(const char *, void *));
extern void *jni_find_native(const char *);
extern void egl_shim_create_window(void);
FILE *stderr_fake;
void *asset_open(const char *p){ (void)p; return NULL; }
typedef int jint; typedef jint (*JNI_OnLoad_t)(void*,void*);
#define SO_NAME "libunity.so"
static void re4_set_import(const char *name, void *fn){
  for(size_t i=0;i<dynlib_numfunctions;i++)
    if(!strcmp(dynlib_functions[i].symbol,name)){ dynlib_functions[i].func=(uintptr_t)fn; return; }
}
static void *N(const char*n){ void*p=jni_find_native(n); fprintf(stderr,"  native %s = %p\n",n,p); return p; }
static long g_re4_key_down_time[256];
static long re4_now_ms(void){
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long)(ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}
static void re4_prepare_injected_key(int action, int keycode, int repeat){
  long now = re4_now_ms();
  long down = now;
  if (keycode >= 0 && keycode < (int)(sizeof(g_re4_key_down_time) / sizeof(g_re4_key_down_time[0]))) {
    if (action == AKEY_EVENT_ACTION_DOWN || g_re4_key_down_time[keycode] <= 0)
      g_re4_key_down_time[keycode] = now;
    down = g_re4_key_down_time[keycode];
    if (action == AKEY_EVENT_ACTION_UP)
      g_re4_key_down_time[keycode] = 0;
  }
  g_hk_inject.action = action;
  g_hk_inject.keycode = keycode;
  g_hk_inject.source = 0x501; /* SOURCE_GAMEPAD | SOURCE_KEYBOARD */
  g_hk_inject.deviceId = 1;   /* = RE4_PAD_DEVICE_ID (mesmo device do MotionEvent) */
  g_hk_inject.metaState = 0;
  g_hk_inject.repeat = repeat;
  g_hk_inject.scancode = keycode;
  g_hk_inject.flags = 0;
  g_hk_inject.unicode = 0;
  g_hk_inject.eventTime = now;
  g_hk_inject.downTime = down;
}
static int re4_inject_key_event(void *env, void *thiz, void *inject,
                                int action, int keycode, int repeat,
                                int frame, const char *origin){
  int handled;
  if (!inject || keycode < 0)
    return 0;
  re4_prepare_injected_key(action, keycode, repeat);
  handled = ((int (*)(void *, void *, void *))inject)(env, thiz, hk_keyevent_object());
  static int padlog = 0;
  if (padlog < 160 || !handled) {
    fprintf(stderr, "[PAD] %s action=%d key=%d handled=%d f=%d\n",
            origin ? origin : "inject", action, keycode, handled, frame);
    if (padlog < 160)
      padlog++;
  }
  return handled;
}
/* Injeta um MotionEvent de gamepad (stick analogico + dpad HAT) via nativeInjectEvent.
   O jogo le os eixos por MotionEvent.getAxisValue (jni_CallFloatMethodV) -> Leon anda. */
static int re4_inject_motion_event(void *env, void *thiz, void *inject,
                                   const FakeInputEvent *ev, int frame){
  if(!inject || !ev) return 0;
  long now = re4_now_ms();
  g_hk_inject.action   = AMOTION_EVENT_ACTION_MOVE;
  g_hk_inject.keycode  = 0;
  g_hk_inject.source   = AINPUT_SOURCE_JOYSTICK;   /* 0x1000010 */
  g_hk_inject.deviceId = 1;                        /* = RE4_PAD_DEVICE_ID (device registrado) */
  g_hk_inject.metaState= 0;
  g_hk_inject.repeat   = 0;
  g_hk_inject.flags    = 0;
  g_hk_inject.eventTime= now;
  g_hk_inject.downTime = now;
  for(int i=0;i<64;i++) g_hk_inject.axes[i] = ev->axes[i];
  int handled = ((int (*)(void *, void *, void *))inject)(env, thiz, hk_motionevent_object());
  static int lg=0;
  if(lg++ < 40)
    fprintf(stderr,"[PAD] MOTION inject hat=(%.2f,%.2f) Lstick=(%.2f,%.2f) Rstick=(%.2f,%.2f) handled=%d f=%d\n",
            ev->axes[AMOTION_EVENT_AXIS_HAT_X], ev->axes[AMOTION_EVENT_AXIS_HAT_Y],
            ev->axes[AMOTION_EVENT_AXIS_X], ev->axes[AMOTION_EVENT_AXIS_Y],
            ev->axes[AMOTION_EVENT_AXIS_Z], ev->axes[AMOTION_EVENT_AXIS_RZ], handled, frame);
  return handled;
}
/* ===== "gotokey": habilita Navigation=Automatic nos botoes do menu (uGUI) via Mono =====
   O menu CODEX usa o StandaloneInputModule padrao (envia Move/Submit lendo GetAxisRaw+
   GetButtonDown que ja hookamos), MAS os botoes tem Navigation=None (menu p/ touch) ->
   a selecao nao anda. Setamos m_Navigation.mode=Automatic em runtime p/ o dpad/analogico
   navegar START/OPTIONS/QUIT. Roda na thread do render (ja attachada ao Mono).
   RE4_NO_MENUNAV desliga. */
static void* re4_mono_sym(const char* nm){
  if(!g_m_mono) return NULL;
  so_module *cur=so_save(); so_use(g_m_mono);
  void* p=(void*)so_find_addr_safe(nm);
  so_use(cur); free(cur);
  return p;
}
static struct {
  int ready;
  void* (*get_root_domain)(void);
  void* (*image_loaded)(const char*);
  void* (*class_from_name)(void*,const char*,const char*);
  void* (*class_get_type)(void*);
  void* (*type_get_object)(void*,void*);
  void* (*class_get_method_from_name)(void*,const char*,int);
  void* (*runtime_invoke)(void*,void*,void**,void**);
  char* (*array_addr_with_size)(void*,int,uintptr_t);
  void* (*class_get_field_from_name)(void*,const char*);
  void  (*field_set_value)(void*,void*,void*);
  void* (*object_unbox)(void*);
} MN;
static int re4_mono_nav_init(void){
  if(MN.ready) return MN.ready>0;
  MN.ready=-1;
  MN.get_root_domain=re4_mono_sym("mono_get_root_domain");
  MN.image_loaded=re4_mono_sym("mono_image_loaded");
  MN.class_from_name=re4_mono_sym("mono_class_from_name");
  MN.class_get_type=re4_mono_sym("mono_class_get_type");
  MN.type_get_object=re4_mono_sym("mono_type_get_object");
  MN.class_get_method_from_name=re4_mono_sym("mono_class_get_method_from_name");
  MN.runtime_invoke=re4_mono_sym("mono_runtime_invoke");
  MN.array_addr_with_size=re4_mono_sym("mono_array_addr_with_size");
  MN.class_get_field_from_name=re4_mono_sym("mono_class_get_field_from_name");
  MN.field_set_value=re4_mono_sym("mono_field_set_value");
  MN.object_unbox=re4_mono_sym("mono_object_unbox");
  if(MN.get_root_domain&&MN.image_loaded&&MN.class_from_name&&MN.class_get_type&&MN.type_get_object
     &&MN.class_get_method_from_name&&MN.runtime_invoke&&MN.array_addr_with_size
     &&MN.class_get_field_from_name&&MN.field_set_value&&MN.object_unbox) MN.ready=1;
  fprintf(stderr,"[MENUNAV] mono init ready=%d\n",MN.ready); fsync(2);
  return MN.ready>0;
}
static void re4_enable_menu_nav(int frame){
  if(getenv("RE4_NO_MENUNAV")) return;
  if(g_gameplay) return;                       /* gameplay -> nao mexe em Mono (anti-freeze) */
  if(frame<60 || (frame % 30)!=0) return;     /* periodico, depois do boot */
  if(!re4_mono_nav_init()) return;
  void* domain=MN.get_root_domain(); if(!domain) return;
  void* ui_img=MN.image_loaded("UnityEngine.UI");
  void* core_img=MN.image_loaded("UnityEngine.CoreModule");
  static int warned=0;
  if(!ui_img||!core_img){ if(warned++<3){fprintf(stderr,"[MENUNAV] img ui=%p core=%p\n",ui_img,core_img);fsync(2);} return; }
  void* sel_cls=MN.class_from_name(ui_img,"UnityEngine.UI","Selectable");
  void* obj_cls=MN.class_from_name(core_img,"UnityEngine","Object");
  if(!sel_cls||!obj_cls){ if(warned++<3){fprintf(stderr,"[MENUNAV] sel_cls=%p obj_cls=%p\n",sel_cls,obj_cls);fsync(2);} return; }
  void* find=MN.class_get_method_from_name(obj_cls,"FindObjectsOfType",1);
  void* navfld=MN.class_get_field_from_name(sel_cls,"m_Navigation");
  if(!find||!navfld){ if(warned++<3){fprintf(stderr,"[MENUNAV] find=%p navfld=%p\n",find,navfld);fsync(2);} return; }
  void* seltype=MN.type_get_object(domain, MN.class_get_type(sel_cls));
  void* params[1]={seltype}; void* exc=NULL;
  void* arr=MN.runtime_invoke(find,NULL,params,&exc);
  if(exc||!arr) return;
  int n=*(int*)((char*)arr+12);   /* MonoArray.max_length (ARM32: obj 8 + bounds 4) */
  if(n<0||n>4096) return;
  static int last_n=-1;
  if(n==last_n) return;        /* mesmo conjunto -> ja aplicado */
  last_n=n;
  unsigned char nav[32]; memset(nav,0,sizeof(nav)); *(int*)nav=3; /* Navigation.Mode.Automatic */
  int changed=0;
  /* p/ logar nomes */
  static void* m_getgo=0; void* comp_cls=MN.class_from_name(core_img,"UnityEngine","Component");
  if(!m_getgo && comp_cls) m_getgo=MN.class_get_method_from_name(comp_cls,"get_gameObject",0);
  static void* m_getname2=0; if(!m_getname2) m_getname2=MN.class_get_method_from_name(obj_cls,"get_name",0);
  for(int i=0;i<n;i++){
    void** slot=(void**)MN.array_addr_with_size(arr,(int)sizeof(void*),(uintptr_t)i);
    void* sel=slot?*slot:NULL;
    if(!sel) continue;
    MN.field_set_value(sel,navfld,nav);
    changed++;
    if(getenv("RE4_SELLOG") && m_getgo && m_getname2 && g_mono_string_to_utf8_fn){
      void* e1=0; void* go=MN.runtime_invoke(m_getgo,sel,NULL,&e1);
      if(!e1 && go){ void* e2=0; void* s=MN.runtime_invoke(m_getname2,go,NULL,&e2);
        if(!e2 && s){ char* u=g_mono_string_to_utf8_fn(s); if(u) fprintf(stderr,"[SELLIST] [%d] '%s'\n",i,u); } } }
  }
  fprintf(stderr,"[MENUNAV] Navigation=Automatic em %d selectables (f=%d)\n",changed,frame); fsync(2);
}
/* Loga o nome do GameObject selecionado no EventSystem -> ver se a navegacao MOVE. */
static void re4_log_selected(int frame){
  if(!getenv("RE4_SELLOG")) return;
  if(frame<60 || (frame%12)!=0) return;
  if(!re4_mono_nav_init()) return;
  void* domain=MN.get_root_domain(); if(!domain) return;
  void* ui_img=MN.image_loaded("UnityEngine.UI"); if(!ui_img) return;
  void* core_img=MN.image_loaded("UnityEngine.CoreModule"); if(!core_img) return;
  static void* es_cls=0; static void* es_inst=0; static void* m_getsel=0; static void* m_getname=0; static void* obj_cls=0;
  if(!es_cls){ es_cls=MN.class_from_name(ui_img,"UnityEngine.EventSystems","EventSystem");
    obj_cls=MN.class_from_name(core_img,"UnityEngine","Object");
    if(es_cls){ m_getsel=MN.class_get_method_from_name(es_cls,"get_currentSelectedGameObject",0); }
    if(obj_cls){ m_getname=MN.class_get_method_from_name(obj_cls,"get_name",0); } }
  if(!es_cls||!m_getsel||!m_getname) return;
  if(!es_inst){ /* acha a instancia do EventSystem */
    void* find=MN.class_get_method_from_name(obj_cls,"FindObjectsOfType",1);
    void* t=MN.type_get_object(domain,MN.class_get_type(es_cls));
    void* p[1]={t}; void* e=0; void* arr=MN.runtime_invoke(find,NULL,p,&e);
    if(arr && !e){ int n=*(int*)((char*)arr+12); if(n>0){ void**s=(void**)MN.array_addr_with_size(arr,(int)sizeof(void*),0); es_inst=s?*s:0; } } }
  if(!es_inst) return;
  void* e=0; void* go=MN.runtime_invoke(m_getsel,es_inst,NULL,&e);
  if(e) return;
  const char* nm="(null)";
  if(go){ void* e2=0; void* s=MN.runtime_invoke(m_getname,go,NULL,&e2);
    if(!e2 && s && g_mono_string_to_utf8_fn){ char* u=g_mono_string_to_utf8_fn(s); if(u){ nm=u; } } }
  static char last[64]={0};
  if(strncmp(last,nm,sizeof(last)-1)){ snprintf(last,sizeof last,"%s",nm);
    fprintf(stderr,"[SELLOG] selecionado='%s' f=%d\n",nm,frame); fsync(2); }
}
/* gotokey DIRETO: forca a selecao de um botao por NOME via EventSystem.SetSelectedGameObject.
   Escreve o nome em /tmp/re4sel (ex "Start","Options","Quit","Back","Game Options").
   Bypassa Navigation/fantasma: eu controlo a selecao. Gated RE4_MENUDRIVE. */
static void re4_menu_drive(int frame){
  if(!getenv("RE4_MENUDRIVE")) return;
  if(frame<60) return;
  if(!re4_mono_nav_init()) return;
  static char want[64]={0}; int invoke=0;
  FILE* f=fopen("/tmp/re4sel","r");
  if(f){ char b[64]={0}; if(fgets(b,sizeof b,f)){ char*nl=strchr(b,'\n'); if(nl)*nl=0; if(b[0]){ snprintf(want,sizeof want,"%s",b); invoke=0; } }
    fclose(f); f=fopen("/tmp/re4sel","w"); if(f)fclose(f); }
  f=fopen("/tmp/re4invoke","r");
  if(f){ char b[64]={0}; if(fgets(b,sizeof b,f)){ char*nl=strchr(b,'\n'); if(nl)*nl=0; if(b[0]){ snprintf(want,sizeof want,"%s",b); invoke=1; } }
    fclose(f); f=fopen("/tmp/re4invoke","w"); if(f)fclose(f); }
  if(!want[0]) return;
  void* domain=MN.get_root_domain(); if(!domain) return;
  void* ui_img=MN.image_loaded("UnityEngine.UI"); void* core_img=MN.image_loaded("UnityEngine.CoreModule");
  if(!ui_img||!core_img) return;
  void* sel_cls=MN.class_from_name(ui_img,"UnityEngine.UI","Selectable");
  void* obj_cls=MN.class_from_name(core_img,"UnityEngine","Object");
  void* comp_cls=MN.class_from_name(core_img,"UnityEngine","Component");
  void* es_cls=MN.class_from_name(ui_img,"UnityEngine.EventSystems","EventSystem");
  if(!sel_cls||!obj_cls||!comp_cls||!es_cls) return;
  void* m_find=MN.class_get_method_from_name(obj_cls,"FindObjectsOfType",1);
  void* m_getgo=MN.class_get_method_from_name(comp_cls,"get_gameObject",0);
  void* m_getname=MN.class_get_method_from_name(obj_cls,"get_name",0);
  void* m_setsel=MN.class_get_method_from_name(es_cls,"SetSelectedGameObject",1);
  if(!m_find||!m_getgo||!m_getname||!m_setsel) return;
  /* instancia do EventSystem */
  void* e=0; void* est=MN.type_get_object(domain,MN.class_get_type(es_cls));
  void* ep[1]={est}; void* esarr=MN.runtime_invoke(m_find,NULL,ep,&e);
  void* es_inst=0; if(esarr&&!e){ int en=*(int*)((char*)esarr+12); if(en>0){ void**s=(void**)MN.array_addr_with_size(esarr,(int)sizeof(void*),0); es_inst=s?*s:0; } }
  if(!es_inst) return;
  /* acha o Selectable pelo nome */
  void* st=MN.type_get_object(domain,MN.class_get_type(sel_cls));
  void* sp[1]={st}; e=0; void* arr=MN.runtime_invoke(m_find,NULL,sp,&e);
  if(!arr||e) return;
  int n=*(int*)((char*)arr+12); if(n<0||n>4096) return;
  void* target_go=0; void* target_sel=0;
  for(int i=0;i<n;i++){
    void**slot=(void**)MN.array_addr_with_size(arr,(int)sizeof(void*),(uintptr_t)i);
    void* sel=slot?*slot:0; if(!sel) continue;
    void* e1=0; void* go=MN.runtime_invoke(m_getgo,sel,NULL,&e1); if(e1||!go) continue;
    void* e2=0; void* s=MN.runtime_invoke(m_getname,go,NULL,&e2); if(e2||!s) continue;
    char* u=g_mono_string_to_utf8_fn?g_mono_string_to_utf8_fn(s):0;
    if(u && !strcasecmp(u,want)){ target_go=go; target_sel=sel; break; }
  }
  if(!target_go){ fprintf(stderr,"[MENUDRIVE] botao '%s' nao encontrado f=%d\n",want,frame); fsync(2); want[0]=0; return; }
  if(!invoke){
    void* pp[1]={target_go}; void* e3=0;
    MN.runtime_invoke(m_setsel,es_inst,pp,&e3);
    fprintf(stderr,"[MENUDRIVE] SetSelectedGameObject('%s') exc=%p f=%d\n",want,e3,frame); fsync(2);
  } else {
    /* invoca o onClick do Button diretamente (bypassa selecao/submit quebrados) */
    static void* btn_cls=0; static void* m_onclick=0; static void* m_uinvoke=0;
    if(!btn_cls){ btn_cls=MN.class_from_name(ui_img,"UnityEngine.UI","Button");
      if(btn_cls) m_onclick=MN.class_get_method_from_name(btn_cls,"get_onClick",0);
      void* ev_cls=MN.class_from_name(core_img,"UnityEngine.Events","UnityEvent");
      if(ev_cls) m_uinvoke=MN.class_get_method_from_name(ev_cls,"Invoke",0); }
    if((!strcasecmp(want,"New")||!strcasecmp(want,"Continue")) && !g_gameplay){ g_gameplay=1; g_gameplay_frame=frame; }
    if(m_onclick && m_uinvoke){
      void* e4=0; void* evt=MN.runtime_invoke(m_onclick,target_sel,NULL,&e4);
      if(!e4 && evt){ void* e5=0; MN.runtime_invoke(m_uinvoke,evt,NULL,&e5);
        fprintf(stderr,"[MENUDRIVE] INVOKE onClick('%s') exc=%p f=%d\n",want,e5,frame); fsync(2); }
      else fprintf(stderr,"[MENUDRIVE] get_onClick('%s') exc=%p evt=%p f=%d\n",want,e4,evt,frame),fsync(2);
    } else fprintf(stderr,"[MENUDRIVE] onclick=%p uinvoke=%p\n",m_onclick,m_uinvoke),fsync(2);
  }
  want[0]=0;
}
/* ===== NAVEGACAO COMPLETA DO MENU (gotokey de producao) =====
   O StandaloneInputModule do jogo nao move a selecao (Navigation=None) e o Submit nao
   aciona; um script ainda varre a selecao p/ o "Back". Entao IGNORAMOS isso e dirigimos
   o menu nos mesmos via Mono: a cada frame achamos os botoes VISIVEIS+interativos,
   ordenamos por Y (topo->baixo), o dpad/analogico move um cursor, A invoca o onClick do
   botao (entra/abre submenu), B invoca o "Back". Le o gamepad de g_re4_gp_btn (fisico ou
   virtual). RE4_NO_MENUNAV2 desliga. */
static int re4_menu_edge(int idx){ return g_re4_gp_btn[idx] && !g_re4_gp_prev[idx]; }
static void re4_menu_nav(int frame){
  if(getenv("RE4_NO_MENUNAV2")) return;
  if(frame<60) return;
  /* ANTI-FREEZE: assim que o gameplay comeca (New/Continue) PARAMOS de mexer no Mono.
     Fazer runtime_invoke/FindObjectsOfType durante o load/gameplay pesado trava a tela. */
  if(g_gameplay){ g_in_menu=0; return; }
  if(!re4_mono_nav_init()) return;
  void* domain=MN.get_root_domain(); if(!domain) return;
  static int res=0; static void* ui_img=0,*core_img=0,*sel_cls=0,*obj_cls=0,*comp_cls=0,*es_cls=0,*btn_cls=0,*ev_cls=0,*go_cls=0,*tr_cls=0;
  static void* m_find=0,*m_getgo=0,*m_getname=0,*m_setsel=0,*m_onclick=0,*m_uinvoke=0,*m_active=0,*m_interact=0,*m_gettr=0,*m_getpos=0;
  if(!res){
    ui_img=MN.image_loaded("UnityEngine.UI"); core_img=MN.image_loaded("UnityEngine.CoreModule");
    if(!ui_img||!core_img) return;
    sel_cls=MN.class_from_name(ui_img,"UnityEngine.UI","Selectable");
    btn_cls=MN.class_from_name(ui_img,"UnityEngine.UI","Button");
    es_cls=MN.class_from_name(ui_img,"UnityEngine.EventSystems","EventSystem");
    obj_cls=MN.class_from_name(core_img,"UnityEngine","Object");
    comp_cls=MN.class_from_name(core_img,"UnityEngine","Component");
    go_cls=MN.class_from_name(core_img,"UnityEngine","GameObject");
    tr_cls=MN.class_from_name(core_img,"UnityEngine","Transform");
    ev_cls=MN.class_from_name(core_img,"UnityEngine.Events","UnityEvent");
    if(!sel_cls||!btn_cls||!es_cls||!obj_cls||!comp_cls||!go_cls||!tr_cls||!ev_cls) return;
    m_find=MN.class_get_method_from_name(obj_cls,"FindObjectsOfType",1);
    m_getgo=MN.class_get_method_from_name(comp_cls,"get_gameObject",0);
    m_getname=MN.class_get_method_from_name(obj_cls,"get_name",0);
    m_setsel=MN.class_get_method_from_name(es_cls,"SetSelectedGameObject",1);
    m_onclick=MN.class_get_method_from_name(btn_cls,"get_onClick",0);
    m_uinvoke=MN.class_get_method_from_name(ev_cls,"Invoke",0);
    m_active=MN.class_get_method_from_name(go_cls,"get_activeInHierarchy",0);
    m_interact=MN.class_get_method_from_name(sel_cls,"IsInteractable",0);
    m_gettr=MN.class_get_method_from_name(comp_cls,"get_transform",0);
    m_getpos=MN.class_get_method_from_name(tr_cls,"get_position",0);
    if(!m_find||!m_getgo||!m_getname||!m_setsel||!m_onclick||!m_uinvoke||!m_active||!m_interact||!m_gettr||!m_getpos) return;
    res=1; fprintf(stderr,"[MENUNAV2] pronto f=%d\n",frame); fsync(2);
  }
  /* es instance (cache) */
  static void* es_inst=0;
  if(!es_inst){ void* t=MN.type_get_object(domain,MN.class_get_type(es_cls)); void* p[1]={t}; void* e=0;
    void* a=MN.runtime_invoke(m_find,NULL,p,&e); if(a&&!e){ int en=*(int*)((char*)a+12); if(en>0){ void**s=(void**)MN.array_addr_with_size(a,(int)sizeof(void*),0); es_inst=s?*s:0; } } }
  if(!es_inst) return;
  /* coleta botoes visiveis+interativos */
  void* st=MN.type_get_object(domain,MN.class_get_type(sel_cls)); void* sp[1]={st}; void* e=0;
  void* arr=MN.runtime_invoke(m_find,NULL,sp,&e); if(!arr||e) return;
  int n=*(int*)((char*)arr+12); if(n<0||n>4096) return;
  void* gos[40]; void* sels[40]; float xs[40]; float ys[40]; int cnt=0; int has_menu=0;
  for(int i=0;i<n && cnt<40;i++){
    void**slot=(void**)MN.array_addr_with_size(arr,(int)sizeof(void*),(uintptr_t)i);
    void* sel=slot?*slot:0; if(!sel) continue;
    void* e1=0; void* go=MN.runtime_invoke(m_getgo,sel,NULL,&e1); if(e1||!go) continue;
    void* e2=0; void* ab=MN.runtime_invoke(m_active,go,NULL,&e2); if(e2||!ab) continue;
    if(!*(unsigned char*)MN.object_unbox(ab)) continue;        /* ativo na hierarquia */
    void* e3=0; void* ib=MN.runtime_invoke(m_interact,sel,NULL,&e3); if(e3||!ib) continue;
    if(!*(unsigned char*)MN.object_unbox(ib)) continue;        /* interativo */
    /* detecta menu: nome de botao tipico (Start/Options/Quit/Back/New/Continue) */
    void* en=0; void* snm=MN.runtime_invoke(m_getname,go,NULL,&en);
    if(!en&&snm&&g_mono_string_to_utf8_fn){ char* u=g_mono_string_to_utf8_fn(snm);
      if(u && (!strcasecmp(u,"Start")||!strcasecmp(u,"Options")||!strcasecmp(u,"Quit")||
               !strcasecmp(u,"Back")||!strcasecmp(u,"New")||!strcasecmp(u,"Continue"))) has_menu=1; }
    float x=0.0f,y=0.0f;
    void* e4=0; void* tr=MN.runtime_invoke(m_gettr,sel,NULL,&e4);
    if(!e4 && tr){ void* e5=0; void* pv=MN.runtime_invoke(m_getpos,tr,NULL,&e5); if(!e5&&pv){ float* fp=(float*)MN.object_unbox(pv); if(fp){ x=fp[0]; y=fp[1]; } } }
    gos[cnt]=go; sels[cnt]=sel; xs[cnt]=x; ys[cnt]=y; cnt++;
  }
  g_in_menu = has_menu;                 /* gameplay (sem botoes de menu) -> 0 */
  if(cnt<=0 || !has_menu){ return; }    /* nao dirige fora do menu (deixa o gameplay) */
  /* cursor RASTREADO POR IDENTIDADE (estavel apesar do auto-ciclo do jogo) */
  static void* cur=0;
  int ci=-1;
  for(int i=0;i<cnt;i++) if(gos[i]==cur){ ci=i; break; }
  if(ci<0){ /* cursor sumiu (mudou de painel) -> pega o mais ao TOPO */
    ci=0; for(int i=1;i<cnt;i++) if(ys[i]>ys[ci]) ci=i; cur=gos[ci]; }
  /* navegacao DIRECIONAL por posicao: acha o vizinho na direcao apertada */
  int up=re4_menu_edge(RE4_BTN_DU), dn=re4_menu_edge(RE4_BTN_DD);
  int lf=re4_menu_edge(RE4_BTN_DL), rt=re4_menu_edge(RE4_BTN_DR);
  int dir_y = up?1:(dn?-1:0);   /* y maior = cima */
  int dir_x = rt?1:(lf?-1:0);
  if(dir_y||dir_x){
    int best=-1; float bestcost=1e18f;
    for(int i=0;i<cnt;i++){ if(i==ci) continue;
      float dx=xs[i]-xs[ci], dy=ys[i]-ys[ci];
      float along = dir_y? dir_y*dy : dir_x*dx;     /* avanco na direcao */
      float perp  = dir_y? dx : dy;                 /* desvio lateral */
      if(along<=1.0f) continue;                     /* tem que ir na direcao */
      float cost = along + 3.0f*(perp<0?-perp:perp);
      if(cost<bestcost){ bestcost=cost; best=i; }
    }
    if(best>=0){ ci=best; cur=gos[ci]; }
  }
  /* destaca (sobrepoe o auto-ciclo do jogo, todo frame) */
  { void* pp[1]={gos[ci]}; void* ee=0; MN.runtime_invoke(m_setsel,es_inst,pp,&ee); }
  /* A ou X = aceitar (invoca onClick); B = voltar (invoca Back) */
  int doA=re4_menu_edge(RE4_BTN_A) || re4_menu_edge(RE4_BTN_X);
  int doB=re4_menu_edge(RE4_BTN_B);
  if(doA){
    /* se o botao for New/Continue (entra no gameplay) -> marca g_gameplay p/ PARAR a poke-Mono */
    void* en2=0; void* snm2=MN.runtime_invoke(m_getname,gos[ci],NULL,&en2);
    if(!en2&&snm2&&g_mono_string_to_utf8_fn){ char* u=g_mono_string_to_utf8_fn(snm2);
      if(u && (!strcasecmp(u,"New")||!strcasecmp(u,"Continue"))){ g_gameplay=1; g_gameplay_frame=frame; fprintf(stderr,"[MENUNAV2] gameplay START (%s) -> para poke-Mono f=%d\n",u,frame); } }
    void* e6=0; void* evt=MN.runtime_invoke(m_onclick,sels[ci],NULL,&e6);
    if(!e6&&evt){ void* e7=0; MN.runtime_invoke(m_uinvoke,evt,NULL,&e7); fprintf(stderr,"[MENUNAV2] A->onClick ci=%d exc=%p f=%d\n",ci,e7,frame); fsync(2); }
    cur=0; /* forca reavaliacao do cursor no proximo frame (painel pode mudar) */
  }
  if(doB){
    for(int i=0;i<cnt;i++){ void* eb=0; void* gg=MN.runtime_invoke(m_getgo,sels[i],NULL,&eb); if(eb||!gg) continue;
      void* eb2=0; void* s=MN.runtime_invoke(m_getname,gg,NULL,&eb2);
      char* u=(!eb2&&s&&g_mono_string_to_utf8_fn)?g_mono_string_to_utf8_fn(s):0;
      if(u && !strcasecmp(u,"Back")){ void* ev2=0; void* evt=MN.runtime_invoke(m_onclick,sels[i],NULL,&ev2);
        if(!ev2&&evt){ void* e9=0; MN.runtime_invoke(m_uinvoke,evt,NULL,&e9); fprintf(stderr,"[MENUNAV2] B->Back f=%d\n",frame); fsync(2);} cur=0; break; } }
  }
  if(getenv("RE4_GPLOG") && (doA||doB||up||dn||lf||rt)){
    fprintf(stderr,"[MENUNAV2] ci=%d/%d up=%d dn=%d lf=%d rt=%d A=%d B=%d f=%d\n",ci,cnt,up,dn,lf,rt,doA,doB,frame); fsync(2);
  }
}
/* Injeta um TOQUE (tap) na tela via nativeInjectEvent (MotionEvent TOUCHSCREEN).
   O menu CODEX do RE4 e TOUCH (jogo mobile): nem KeyEvent nem icall navegam.
   getX/getY sao servidos de axes[0]/axes[1]; getSource de g_hk_inject.source. */
static int re4_inject_touch(void *env, void *thiz, void *inject, int action, float x, float y, int frame){
  if(!inject) return 0;
  long now = re4_now_ms();
  memset(&g_hk_inject.axes, 0, sizeof(g_hk_inject.axes));
  g_hk_inject.action   = action;
  g_hk_inject.keycode  = 0;
  g_hk_inject.source   = AINPUT_SOURCE_TOUCHSCREEN;  /* 0x1002 */
  g_hk_inject.deviceId = 0;                          /* touchscreen device 0 */
  g_hk_inject.metaState= 0;
  g_hk_inject.repeat   = 0;
  g_hk_inject.flags    = 0;
  g_hk_inject.eventTime= now;
  g_hk_inject.downTime = now;
  g_hk_inject.axes[0]  = x;  /* getX */
  g_hk_inject.axes[1]  = y;  /* getY */
  int handled = ((int (*)(void *, void *, void *))inject)(env, thiz, hk_motionevent_object());
  fprintf(stderr,"[TOUCH] action=%d x=%.0f y=%.0f handled=%d f=%d\n",action,x,y,handled,frame);
  fsync(2);
  return handled;
}
static void re4_pump_sdl_input(void *env, void *thiz, void *inject, int frame){
  FakeInputEvent ev;
  android_shim_pump_sdl_events();
  re4_gp_poll();
  /* CONFLITO RESOLVIDO 2026-06-17: havia DOIS caminhos de input competindo -> o
     android_shim injetava uma TEMPESTADE de DPAD KeyEvents (nativeInjectEvent) ENQUANTO
     nossos hooks de icall (GetAxisRaw/GetButton, alimentados por re4_gp_poll) tambem
     entregavam o mesmo input. O menu (StandaloneInputModule) le pelo ICALL; a injecao do
     android_shim era redundante e FLOODAVA a navegacao -> a selecao ciclava ate o "Back".
     Por padrao DESLIGAMOS a injecao do android_shim (so drenamos a fila). RE4_SHIMINJECT=1
     reativa (caminho legado p/ gameplay por KeyEvent). */
  /* android_shim injection DESLIGADA por padrao (2026-06-17): com controle FISICO, injetar
     os eventos dele via nativeInjectEvent durante o load do gameplay TRAVAVA a tela. Nao e
     necessaria: menu=re4_menu_nav, gameplay=touch-move. RE4_SHIMINJECT=1 reativa (debug). */
  int shiminject = getenv("RE4_SHIMINJECT")!=NULL;
  while (android_shim_pop_input_event(&ev)) {
    if(!shiminject) continue;   /* drena sem injetar */
    if (ev.type == AINPUT_EVENT_TYPE_KEY) {
      re4_inject_key_event(env, thiz, inject, ev.action, ev.keycode, 0, frame, "SDL");
      continue;
    }
    if (ev.type == AINPUT_EVENT_TYPE_MOTION && ev.source == AINPUT_SOURCE_JOYSTICK) {
      re4_gp_apply_motion_event(&ev);
      re4_inject_motion_event(env, thiz, inject, &ev, frame);  /* stick/dpad -> getAxisValue */
      continue;
    }
  }
  /* TESTE auto-contido (sem controle fisico): RE4_TESTMOVE injeta stick esq pra frente
     em ciclos -> valida o caminho de movimento via fb0 (Leon/camera mudam). */
  if (getenv("RE4_TESTMOVE") && frame > 120) {
    int per = re4_int_env("RE4_TESTMOVE_PERIOD", 240, 30, 2000);
    int ph = frame % per;
    FakeInputEvent m; memset(&m, 0, sizeof(m));
    m.type = AINPUT_EVENT_TYPE_MOTION; m.source = AINPUT_SOURCE_JOYSTICK;
    m.action = AMOTION_EVENT_ACTION_MOVE;
    if (ph < per/2) { m.axes[AMOTION_EVENT_AXIS_Y] = -1.0f; m.axes[AMOTION_EVENT_AXIS_HAT_Y] = -1.0f; } /* frente */
    re4_inject_motion_event(env, thiz, inject, &m, frame);
  }
  /* TESTE de TECLA: RE4_TESTKEY=<keycode> segura a tecla em ciclos (DOWN metade, UP metade)
     -> descobre se o jogo move/age por KeyEvent (ex: 19=DPAD_UP) via fb0. */
  { const char *tk=getenv("RE4_TESTKEY");
    if(tk&&tk[0]&&frame>120){ int key=atoi(tk);
      int per=re4_int_env("RE4_TESTKEY_PERIOD",180,30,2000); int ph=frame%per;
      static int held=-1;
      if(ph==0 && held!=key){ re4_inject_key_event(env,thiz,inject,AKEY_EVENT_ACTION_DOWN,key,0,frame,"TESTKEY"); held=key; }
      else if(ph==per/2 && held==key){ re4_inject_key_event(env,thiz,inject,AKEY_EVENT_ACTION_UP,key,0,frame,"TESTKEY"); held=-1; }
      else if(held==key){ re4_inject_key_event(env,thiz,inject,AKEY_EVENT_ACTION_DOWN,key,1,frame,"TESTKEY"); } /* repeat=hold */
    }
  }
  /* INJETOR DE TECLA ANDROID AO VIVO: escreve um keycode em /tmp/re4key -> injeta
     DOWN agora + UP alguns frames depois (RE4_LIVEKEY_HOLD). Testa se o menu/jogo
     responde ao caminho Android KeyEvent (nativeInjectEvent). Gated RE4_LIVEKEY. */
  if(getenv("RE4_LIVEKEY")){
    static int pend_key=-1, pend_up_frame=-1;
    if(pend_key>=0 && frame>=pend_up_frame){
      re4_inject_key_event(env,thiz,inject,AKEY_EVENT_ACTION_UP,pend_key,0,frame,"LIVEKEY");
      pend_key=-1;
    }
    if(pend_key<0){
      FILE *kf=fopen("/tmp/re4key","r");
      if(kf){ int k=-1; if(fscanf(kf,"%d",&k)==1 && k>=0){ fclose(kf); kf=fopen("/tmp/re4key","w"); if(kf)fclose(kf);
          int hold=re4_int_env("RE4_LIVEKEY_HOLD",3,1,60);
          re4_inject_key_event(env,thiz,inject,AKEY_EVENT_ACTION_DOWN,k,0,frame,"LIVEKEY");
          pend_key=k; pend_up_frame=frame+hold;
        } else fclose(kf);
      }
    }
  }
  /* INJETOR DE TOQUE AO VIVO: escreve "x y" em /tmp/re4touch -> tap (DOWN+MOVE+UP).
     Testa se o menu CODEX (touch) responde. Gated RE4_LIVETOUCH. */
  if(getenv("RE4_LIVETOUCH")){
    static int st=0; static float tx,ty; static int up_frame=-1;
    if(st==0){
      FILE *tf=fopen("/tmp/re4touch","r");
      if(tf){ float x=-1,y=-1; if(fscanf(tf,"%f %f",&x,&y)==2 && x>=0){ fclose(tf); tf=fopen("/tmp/re4touch","w"); if(tf)fclose(tf);
          tx=x; ty=y; re4_inject_touch(env,thiz,inject,AMOTION_EVENT_ACTION_DOWN,tx,ty,frame);
          st=1; up_frame=frame+re4_int_env("RE4_LIVETOUCH_HOLD",4,1,60);
        } else fclose(tf);
      }
    } else if(st==1){
      if(frame>=up_frame){ re4_inject_touch(env,thiz,inject,AMOTION_EVENT_ACTION_UP,tx,ty,frame); st=0; }
      else re4_inject_touch(env,thiz,inject,AMOTION_EVENT_ACTION_MOVE,tx,ty,frame);
    }
  }
  /* MOVIMENTO NO GAMEPLAY (Leon anda): o gameplay e TOUCH (dpad na tela inf-esq). Traduzimos
     a direcao do gamepad (dpad ou analogico esq) num ARRASTO de toque sobre esse dpad virtual.
     So no gameplay (!g_in_menu). Centro/raio tunaveis (RE4_DPAD_CX/CY/R). RE4_NO_TOUCHMOVE desliga. */
  /* MOVIMENTO DO LEON: o dpad da tela le Input.GetTouch -> alimentamos o TOQUE VIRTUAL
     (g_gp_t*) sobre o dpad conforme o analogico/dpad do gamepad. So no gameplay, depois da
     cena carregar (settle 180f). Centro/raio tunaveis (RE4_DPAD_CX/CY/R). RE4_NO_TOUCHMOVE desliga. */
  g_gp_tprev = g_gp_tdown;
  if(getenv("RE4_TOUCHMOVE") && !g_in_menu && g_gameplay && frame > g_gameplay_frame+180){
    float cx=(float)re4_int_env("RE4_DPAD_CX",80,0,1920);
    float cy=(float)re4_int_env("RE4_DPAD_CY",500,0,1080);
    float R =(float)re4_int_env("RE4_DPAD_R",55,10,400);
    float dx = (g_re4_gp_btn[RE4_BTN_DR]?1.0f:0.0f) - (g_re4_gp_btn[RE4_BTN_DL]?1.0f:0.0f) + g_re4_gp_lx;
    float dy = (g_re4_gp_btn[RE4_BTN_DD]?1.0f:0.0f) - (g_re4_gp_btn[RE4_BTN_DU]?1.0f:0.0f) + g_re4_gp_ly; /* tela: baixo=+ */
    if(dx>1)dx=1; if(dx<-1)dx=-1; if(dy>1)dy=1; if(dy<-1)dy=-1;
    float mag = dx*dx+dy*dy;
    if(mag>0.09f){   /* direcao ativa */
      if(!g_gp_tdown){ g_gp_tx=cx; g_gp_ty=cy; }   /* Began = ancora no centro */
      else { g_gp_tx=cx+dx*R; g_gp_ty=cy+dy*R; }   /* Moved = arrasta na direcao */
      g_gp_tdown=1;
      if(getenv("RE4_GPLOG")){ static int lg=0; if(lg++<60){fprintf(stderr,"[MOVE] vtouch x=%.0f y=%.0f dx=%.2f dy=%.2f f=%d\n",g_gp_tx,g_gp_ty,dx,dy,frame);fsync(2);} }
    } else g_gp_tdown=0;
  } else g_gp_tdown=0;
}
static void re4_autotap(void *env, void *thiz, void *inject, int frame){
  static int tapkey = -2;
  if (tapkey == -2) {
    const char *value = getenv("RE4_AUTOTAP");
    tapkey = (value && value[0]) ? atoi(value) : -1;
    if (tapkey > 0 && inject)
      fprintf(stderr, "[AUTOTAP] keycode=%d via nativeInjectEvent=%p\n", tapkey, inject);
  }
  if (tapkey <= 0 || !inject || frame <= 120)
    return;
  int period = re4_int_env("RE4_AUTOTAP_PERIOD", 90, 10, 600);
  int hold = re4_int_env("RE4_AUTOTAP_HOLD", 3, 1, 60);
  int phase = frame % period;
  if (phase == 0)
    re4_inject_key_event(env, thiz, inject, AKEY_EVENT_ACTION_DOWN, tapkey, 0, frame, "AUTOTAP");
  else if (phase == hold)
    re4_inject_key_event(env, thiz, inject, AKEY_EVENT_ACTION_UP, tapkey, 0, frame, "AUTOTAP");
}
/* SEQUENCIA de teclas em frames especificos: RE4_KEYSEQ="frame:key,frame:key,..."
   Cada entrada injeta DOWN no frame e UP em frame+hold (RE4_KEYSEQ_HOLD, default 4).
   Permite navegar o menu de forma deterministica (ex: 200:108 passa o press-any-key,
   900:23 confirma START). keycodes Android: 19=UP 20=DOWN 21=LEFT 22=RIGHT 23=DPAD_CENTER
   66=ENTER 96=BUTTON_A 108=BUTTON_START 4=BACK. */
#define RE4_SEQ_MAX 64
static void re4_keyseq(void *env, void *thiz, void *inject, int frame){
  static int parsed=0; static int sf[RE4_SEQ_MAX], sk[RE4_SEQ_MAX]; static int sn=0; static int hold=4;
  if(!parsed){ parsed=1;
    const char *s=getenv("RE4_KEYSEQ");
    const char *h=getenv("RE4_KEYSEQ_HOLD"); if(h&&h[0]){ hold=atoi(h); if(hold<1)hold=4; }
    if(s&&s[0]){ const char *p=s;
      while(*p && sn<RE4_SEQ_MAX){ int fr=atoi(p); const char *c=strchr(p,':'); if(!c)break;
        int kc=atoi(c+1); sf[sn]=fr; sk[sn]=kc; sn++;
        const char *comma=strchr(c,','); if(!comma)break; p=comma+1; }
      fprintf(stderr,"[KEYSEQ] %d entradas hold=%d\n",sn,hold);
    }
  }
  if(sn<=0||!inject) return;
  for(int i=0;i<sn;i++){
    if(frame==sf[i]) re4_inject_key_event(env,thiz,inject,AKEY_EVENT_ACTION_DOWN,sk[i],0,frame,"KEYSEQ");
    else if(frame==sf[i]+hold) re4_inject_key_event(env,thiz,inject,AKEY_EVENT_ACTION_UP,sk[i],0,frame,"KEYSEQ");
  }
}
/* BRIDGE de TLS bionic auto-contido: as keys do engine viram SLOTS minhas (1 glibc key so
   guarda o array por-thread). O engine nunca toca o pthread_key do glibc -> sem corrupcao. */
#define NSLOT 1024
static pthread_key_t g_tls_base; static int g_tls_init=0;
static int g_slot_next=1; static pthread_mutex_t g_slot_mtx=PTHREAD_MUTEX_INITIALIZER;
static void tls_dtor(void *p){ free(p); }
static void tls_ensure(void){ if(!g_tls_init){ pthread_key_create(&g_tls_base,tls_dtor); g_tls_init=1; } }
static void **tls_slots(void){ tls_ensure(); void **s=(void**)pthread_getspecific(g_tls_base);
  if(!s){ s=(void**)calloc(NSLOT,sizeof(void*)); pthread_setspecific(g_tls_base,s); } return s; }
static int sh_key_create(pthread_key_t *k, void(*d)(void*)){ static int kc=0; if(kc++<8)fprintf(stderr,"[TLS] key_create dtor=%p\n",d); pthread_mutex_lock(&g_slot_mtx);
  int n=g_slot_next++; pthread_mutex_unlock(&g_slot_mtx); if(n>=NSLOT) return 11; *k=(pthread_key_t)n; return 0; }
static int sh_key_delete(pthread_key_t k){ fprintf(stderr,"[TLS] key_delete %d (no-op)\n",(int)k); return 0; }
static void map_caller(const char*tag,unsigned long ra);
static void *sh_getspecific(pthread_key_t k){ if((int)k<=0||(int)k>=NSLOT){ static int g=0; if(g++<30)fprintf(stderr,"[TLS-GET-BADKEY] k=%d tid=%ld\n",(int)k,(long)pthread_self()); return NULL; } void**arr=tls_slots(); void*v=arr[(int)k];
  if(!v){static int g2=0; if(((int)k==7||(int)k==15) && g2++<4){ fprintf(stderr,"[TLS-GET-NULL] k=%d tid=%p stackscan:\n",(int)k,(void*)pthread_self());
    unsigned long sp; __asm__ volatile("mov %0, sp":"=r"(sp)); unsigned long mb=g_mono_base,ub=(unsigned long)text_virtbase; int hits=0;
    for(int i=0;i<400 && hits<14;i++){ unsigned long w=*(unsigned long*)(sp+i*4);
      if(mb&&w>=mb&&w<mb+0x600000){ fprintf(stderr,"   libmono+0x%lx\n",w-mb); hits++; }
      else if(w>=ub&&w<ub+0x2000000){ fprintf(stderr,"   unity+0x%lx\n",w-ub); hits++; } } }} return v; }
static int sh_setspecific(pthread_key_t k, const void *v){ if((int)k<=0||(int)k>=NSLOT) return 22; void**arr=tls_slots(); if(v){static int st=0; if(st++<2000)fprintf(stderr,"[TLS-SET] k=%d v=%p tid=%p arr=%p\n",(int)k,v,(void*)pthread_self(),(void*)arr);} arr[(int)k]=(void*)v; return 0; }
/* __android_log REAL -> stderr (sem isso, o erro do engine antes do abort some) */
/* RE4_QUIETLOG: no-op (testa se o crash em strchrnul/vfprintf vem do nosso __android_log) */
static int my_alog_print(int prio,const char*tag,const char*fmt,...){ if(getenv("RE4_QUIETLOG"))return 0; va_list ap; va_start(ap,fmt);
  fprintf(stderr,"[ALOG:%d %s] ",prio,tag?tag:"?"); vfprintf(stderr,fmt,ap); fprintf(stderr,"\n"); va_end(ap); return 0; }
static int my_alog_write(int prio,const char*tag,const char*msg){ if(getenv("RE4_QUIETLOG"))return 0; fprintf(stderr,"[ALOG:%d %s] %s\n",prio,tag?tag:"?",msg?msg:""); return 0; }
static int my_alog_vprint(int prio,const char*tag,const char*fmt,va_list ap){ if(getenv("RE4_QUIETLOG"))return 0; fprintf(stderr,"[ALOG:%d %s] ",prio,tag?tag:"?"); vfprintf(stderr,fmt,ap); fprintf(stderr,"\n"); return 0; }
/* bloqueia o engine de instalar handler de crash p/ SIGSEGV/ABRT/etc -> MEU handler pega o crash REAL */
static int my_sigaction(int sig,const void*act,void*old){
  if(getenv("RE4_NOSIGH")&&(sig==4||sig==5||sig==6||sig==7||sig==8||sig==11)){ return 0; } /* debug: deixa o segv original chegar no gdb */ (void)old;
  if(!act) return sigaction(sig,NULL,NULL);
  void *h=*(void* const*)act; /* sa_handler/sa_sigaction @ offset 0 (bionic==glibc) */
  struct sigaction g; memset(&g,0,sizeof g);
  g.sa_sigaction=(void(*)(int,siginfo_t*,void*))h; sigemptyset(&g.sa_mask); g.sa_flags=SA_SIGINFO|SA_RESTART;
  static int sn=0; if(sn++<12) fprintf(stderr,"[SIGACT] sig=%d handler=%p (struct bionic->glibc)\n",sig,h);
  int rr=sigaction(sig,&g,NULL); return rr<0?0:rr; }
/* mapeia um endereco de retorno -> "libmono+0x.." ou "unity+0x.." pra identificar o caller */
static void map_caller(const char*tag,unsigned long ra){
  unsigned long ub=(unsigned long)text_virtbase;
  if(g_mono_base && ra>=g_mono_base && ra<g_mono_base+0x600000) fprintf(stderr,"%s caller=libmono+0x%lx\n",tag,ra-g_mono_base);
  else if(ra>=ub && ra<ub+0x2000000) fprintf(stderr,"%s caller=unity+0x%lx\n",tag,ra-ub);
  else fprintf(stderr,"%s caller=0x%lx (?)\n",tag,ra);
  fflush(stderr);
}
/* intercepta abort/raise/pthread_kill/gsignal: loga o caller + (gated) NAO mata -> vejo o pos-fatal.
   RE4_SUPPRESS_RAISE=1 -> ignora o sinal (testa se o "fatal" e' obrigatorio ou se da pra seguir). */
static int my_raise(int sig){ map_caller("[RAISE]",(unsigned long)__builtin_return_address(0)); fprintf(stderr,"[RAISE] sig=%d\n",sig);
  if(getenv("RE4_SUPPRESS_RAISE")) return 0; return raise(sig); }
static void my_abort(void){ map_caller("[ABORT]",(unsigned long)__builtin_return_address(0));
  if(getenv("RE4_SUPPRESS_RAISE")) return; abort(); }
static int my_ptkill(unsigned long t,int sig){ (void)t; map_caller("[PTKILL]",(unsigned long)__builtin_return_address(0)); fprintf(stderr,"[PTKILL] sig=%d\n",sig);
  if(getenv("RE4_SUPPRESS_RAISE")) return 0; return pthread_kill((pthread_t)t,sig); }
/* loga quem chama exit/_exit (Unity/Mono pode sair limpo num erro fatal sem abort/raise) */
void my_exit(int code){ map_caller("[EXIT]",(unsigned long)__builtin_return_address(0)); fprintf(stderr,"[EXIT] code=%d\n",code); fflush(stderr);
  if(getenv("RE4_SUPPRESS_EXIT")){ static int n=0; if(n++<3)fprintf(stderr,"[EXIT-SUPPRESS]\n"); return; } _exit(code); }
extern void *text_virtbase;
static void on_segv(int sig, siginfo_t *si, void *uc_){
  ucontext_t *uc=(ucontext_t*)uc_;
  unsigned long pc=uc->uc_mcontext.arm_pc, lr=uc->uc_mcontext.arm_lr;
  unsigned long base=(unsigned long)text_virtbase;
  fprintf(stderr,"\n[SEGV] fault=%p pc=0x%lx lr=0x%lx",si->si_addr,pc,lr);
  if(pc>=base && pc<base+0x2000000) fprintf(stderr," (unity+0x%lx)",pc-base);
  if(g_mono_base && pc>=g_mono_base && pc<g_mono_base+0x600000) fprintf(stderr," (libmono+0x%lx)",pc-g_mono_base);
  if(g_mono_base && lr>=g_mono_base && lr<g_mono_base+0x600000) fprintf(stderr," (lr=libmono+0x%lx)",lr-g_mono_base);
  fprintf(stderr," sig=%d\n",sig);
  /* engole SIGABRT (Boehm ABORT) -> tenta seguir em frente (nao parar no 1o erro) */
  if(getenv("RE4_EATABRT") && sig==SIGABRT){ static int ac=0; if(ac++<200){ if(ac<12)fprintf(stderr,"[ABRT-SWALLOW %d]\n",ac); return; } }
  FILE *m=fopen("/proc/self/maps","r"); char ln[300];
  while(m && fgets(ln,sizeof ln,m)){ unsigned long a,b; if(sscanf(ln,"%lx-%lx",&a,&b)==2 && pc>=a && pc<b){ fprintf(stderr,"[SEGV-LIB] %s",ln); break; } }
  if(m) fclose(m);
  static int sc=0; if(sc++) _exit(139);
  fprintf(stderr,"[REGS] r0=0x%lx r1=0x%lx r2=0x%lx r3=0x%lx r4=0x%lx\n",
    uc->uc_mcontext.arm_r0,uc->uc_mcontext.arm_r1,uc->uc_mcontext.arm_r2,uc->uc_mcontext.arm_r3,uc->uc_mcontext.arm_r4);
  unsigned long sp=uc->uc_mcontext.arm_sp;
  fprintf(stderr,"[BACKTRACE frames sp..+8k]\n");
  for(int k=0;k<256;k++){ unsigned long v=*(unsigned long*)(sp+k*4);
    if(v>=base && v<base+0x2000000) fprintf(stderr,"  unity+0x%lx\n",v-base);
    else if(g_mono_base && v>=g_mono_base && v<g_mono_base+0x600000) fprintf(stderr,"  libmono+0x%lx\n",v-g_mono_base); }
  _exit(139);
}
/* ---- SOM: output do FMOD (AudioTrack Java) bombeado em C -> SDL -> Pulse/PipeWire/ALSA ----
 * O FMOD do Unity, no Android, usa o output AUDIOTRACK: uma thread Java chamaria
 * org.fmod.FMODAudioDevice.fmodProcess(ByteBuffer) repetidamente p/ encher PCM e escrever no
 * AudioTrack. Nós damos o device/buffer fake (jni_shim) e bombeamos fmodProcess AQUI, numa
 * thread C, enfileirando EXATAMENTE a capacidade do ByteBuffer no SDL (back-pressure mantem o
 * ritmo = consumo real-time; fmodProcess É o clock do mixer). Backend SDL = auto (device NULL).
 * fmodProcess retorna 0 em SUCESSO (enche o buffer); <0 = output ainda nao pronto. */
static volatile int g_fmod_run = 0;
static void *g_fmod_env = NULL;
static void *volatile g_fmodProcess = NULL;   /* setado quando pronto; callback usa */
static void *g_fmod_bb = NULL, *g_fmod_pcm_p = NULL;
static int   g_fmod_bytes = 4096;
static long  g_fmod_fdev = 0xFAD;             /* this (FMODAudioDevice) fake */
static unsigned long g_audio_cb_n = 0, g_audio_fed = 0;
/* SDL chama isto na thread de audio dele (~a cada `samples` frames) e nos enchemos o stream
   chamando fmodProcess (o mixer do FMOD). PULL-model: o ritmo é ditado pelo SDL/pulse, sem
   busy-wait nem fila -> imune à contenção de CPU do render (degrada pra silêncio sob carga,
   sem travar). */
static void re4_audio_cb(void *ud, Uint8 *stream, int len){
  (void)ud;
  void *fp = g_fmodProcess;
  if (!fp) { memset(stream, 0, len); return; }
  int off = 0;
  while (off < len) {
    int chunk = g_fmod_bytes; if (chunk > len - off) chunk = len - off;
    int r = ((int(*)(void*,void*,void*))fp)(g_fmod_env, &g_fmod_fdev, g_fmod_bb);
    if (r == 0) memcpy(stream + off, g_fmod_pcm_p, chunk);
    else        memset(stream + off, 0, chunk);
    off += chunk; g_audio_fed += chunk;
  }
  if (g_audio_cb_n < 3 || g_audio_cb_n % 500 == 0)
    fprintf(stderr,"[AUDIO] cb #%lu len=%d fed=%lu\n", g_audio_cb_n, len, g_audio_fed);
  g_audio_cb_n++;
}
static void *fmod_audio_thread(void *arg){
  (void)arg;
  void *fp = NULL;
  while (g_fmod_run && !(fp = jni_find_native("fmodProcess"))) usleep(20000);
  if (!fp) { fprintf(stderr,"[AUDIO] fmodProcess nunca registrado -> sem som\n"); return NULL; }
  void *fi = jni_find_native("fmodGetInfo");
  g_fmod_bb    = jni_fmod_bytebuffer();
  g_fmod_pcm_p = jni_fmod_pcm();
  g_fmod_bytes = jni_fmod_pcm_size();   /* = g_fmod_cap (capacidade do DirectByteBuffer) */
  /* fmodGetInfo(i): 0=samplerate, [1]/[2] = buffer/contagem no FMOD mobile. Loga p/ ajuste. */
  unsigned rate = 24000, ch = 2;
  if (fi) {
    int r0 = ((int(*)(void*,void*,int))fi)(g_fmod_env,&g_fmod_fdev,0);
    int r1 = ((int(*)(void*,void*,int))fi)(g_fmod_env,&g_fmod_fdev,1);
    int r2 = ((int(*)(void*,void*,int))fi)(g_fmod_env,&g_fmod_fdev,2);
    fprintf(stderr,"[AUDIO] fmodGetInfo: [0]=%d [1]=%d [2]=%d\n", r0, r1, r2);
    if (r0 >= 8000 && r0 <= 192000) rate = (unsigned)r0;
  }
  if (getenv("RE4_AUDIO_RATE")) rate = (unsigned)atoi(getenv("RE4_AUDIO_RATE"));
  if (getenv("RE4_AUDIO_CH"))   ch   = (unsigned)atoi(getenv("RE4_AUDIO_CH"));
  if (rate < 8000 || rate > 192000) rate = 24000;
  if (ch != 1 && ch != 2) ch = 2;
  g_fmodProcess = fp;   /* arma o callback ANTES de abrir o device */
  SDL_AudioSpec want, have; memset(&want,0,sizeof want);
  want.freq = rate; want.format = AUDIO_S16SYS; want.channels = ch;
  want.samples = (Uint16)(g_fmod_bytes / (ch * 2));   /* = 1024 -> len casa com g_fmod_bytes */
  want.callback = re4_audio_cb;
  if (!SDL_WasInit(SDL_INIT_AUDIO)) SDL_InitSubSystem(SDL_INIT_AUDIO);
  SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
  if (dev) SDL_PauseAudioDevice(dev, 0);
  const char *drv = SDL_GetCurrentAudioDriver();
  fprintf(stderr,"[AUDIO] SDL dev=%d driver=%s rate=%u ch=%u bytes=%d samples=%d (have f=%d c=%d s=%d) err=%s\n",
          dev, drv?drv:"(null)", rate, ch, g_fmod_bytes, want.samples,
          have.freq, have.channels, have.samples, dev ? "" : SDL_GetError());
  /* o device fica tocando via callback; a thread só mantém o escopo vivo */
  while (g_fmod_run) usleep(200000);
  if (dev) SDL_CloseAudioDevice(dev);
  return NULL;
}

int main(void){
  const char *pkg=getenv("RE4_PACKAGE_NAME");
  const char *obb=getenv("RE4_OBB_VERSION");
  int obb_version=(obb&&obb[0])?atoi(obb):1;
  struct sigaction sa; memset(&sa,0,sizeof sa); sa.sa_sigaction=on_segv; sa.sa_flags=SA_SIGINFO;
  sigaction(SIGSEGV,&sa,0); sigaction(SIGBUS,&sa,0); sigaction(SIGABRT,&sa,0); sigaction(SIGILL,&sa,0); sigaction(SIGTRAP,&sa,0); sigaction(SIGFPE,&sa,0);
  /* Mapeia uma pagina no endereco 0 cheia de 'bx lr' -> chamadas via ponteiro NULL (pc=0)
     viram no-op (retornam) em vez de crashar -> o programa segue + revela o proximo passo. */
  if(getenv("RE4_NULLPAGE")){ void *z=mmap((void*)0,4096,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0);
    if(z==(void*)0){ for(int i=0;i<1024;i++) *(volatile unsigned*)(uintptr_t)(i*4)=0xe12fff1eu; __builtin___clear_cache((char*)0,(char*)4096); fprintf(stderr,"[NULLPAGE] mapeada em 0 (bx lr)\n"); }
    else { fprintf(stderr,"[NULLPAGE] FALHOU (z=%p errno=%d)\n",z,errno); if(z!=MAP_FAILED)munmap(z,4096); } }
  fprintf(stderr,"=== RE4 Unity 2018 (ARM32 GLES2) ===\n");
  /* libz: a Unity importa inflate/inflateEnd/inflateInit2_ p/ DESCOMPRIMIR assets/cena.
     Sem libz no namespace, caiam no stub -> descompressao vazia -> o load assincrono nunca
     completa -> a UnityMain trava esperando a fila de jobs. dlopen RTLD_GLOBAL torna inflate
     visivel p/ dlsym(RTLD_DEFAULT) no re4_resolve. */
  { void *z=dlopen("libz.so.1",RTLD_NOW|RTLD_GLOBAL); if(!z)z=dlopen("libz.so",RTLD_NOW|RTLD_GLOBAL);
    fprintf(stderr,"[LIBZ] dlopen -> %p (inflate=%p)\n",z,dlsym(RTLD_DEFAULT,"inflate")); }
  /* GL: Unity resolve glClear/glDrawArrays/etc via dlsym(RTLD_DEFAULT) -> sem libGLESv2 no
     namespace global, viram NULL -> crash ao chamar. dlopen RTLD_GLOBAL torna-as visiveis. */
  { void *g=dlopen("libGLESv2.so.2",RTLD_NOW|RTLD_GLOBAL); if(!g)g=dlopen("libGLESv2.so",RTLD_NOW|RTLD_GLOBAL);
    void *e=dlopen("libEGL.so.1",RTLD_NOW|RTLD_GLOBAL); if(!e)e=dlopen("libEGL.so",RTLD_NOW|RTLD_GLOBAL);
    fprintf(stderr,"[LIBGL] GLESv2=%p EGL=%p (glClear=%p)\n",g,e,dlsym(RTLD_DEFAULT,"glClear")); }
  size_t hs=48*1024*1024;
  void *heap=mmap(NULL,hs,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  if(heap==MAP_FAILED){perror("mmap");return 1;}
  if(so_load(SO_NAME,heap,hs)<0){fprintf(stderr,"load FALHOU\n");return 1;}
  if(so_relocate()<0){fprintf(stderr,"reloc FALHOU\n");return 1;}
  re4_fill(); recon_wire_pthread(re4_set_import);
  re4_set_import("pthread_key_create",(void*)sh_key_create);
  re4_set_import("pthread_key_delete",(void*)sh_key_delete);
  re4_set_import("pthread_getspecific",(void*)sh_getspecific);
  re4_set_import("pthread_setspecific",(void*)sh_setspecific);
  re4_set_import("__system_property_get",(void*)my_sysprop);
  re4_set_import("__android_log_print",(void*)my_alog_print);
  re4_set_import("__android_log_write",(void*)my_alog_write);
  re4_set_import("__android_log_vprint",(void*)my_alog_vprint);
  re4_set_import("abort",(void*)my_abort);
  re4_set_import("fopen",(void*)my_fopen);
  re4_set_import("open",(void*)my_open);
  re4_set_import("write",(void*)my_write);
  re4_set_import("read",(void*)my_read);
  re4_set_import("fstatat64",(void*)my_fstatat64);
  re4_set_import("newfstatat",(void*)my_fstatat64);
  re4_set_import("fstat64",(void*)my_fstat64);
  re4_set_import("fstat",(void*)my_fstat64);
  re4_set_import("stat64",(void*)my_stat64);
  re4_set_import("stat",(void*)my_stat64);
  re4_set_import("lstat64",(void*)my_lstat64);
  re4_set_import("access",(void*)my_access);
  re4_set_import("unlink",(void*)my_unlink);
  re4_set_import("remove",(void*)my_unlink);
  re4_set_import("rename",(void*)my_rename);
  re4_set_import("mkdir",(void*)my_mkdir);
  re4_set_import("raise",(void*)my_raise);
  re4_set_import("pthread_kill",(void*)my_ptkill);
  re4_set_import("sigaction",(void*)my_sigaction);
  re4_set_import("dlopen",(void*)my_dlopen);
  re4_set_import("dlsym",(void*)my_dlsym);
  re4_set_import("getpwuid",(void*)my_getpwuid);
  re4_set_import("getpwnam",(void*)my_getpwuid);
  re4_set_import("dlerror",(void*)my_dlerror);
  re4_set_import("dladdr",(void*)my_dladdr);
  re4_set_import("dlclose",(void*)my_dlclose);
  re4_set_import("ANativeWindow_fromSurface",(void*)my_aw_fromSurface);
  re4_set_import("ANativeWindow_setBuffersGeometry",(void*)my_aw_setgeom);
  re4_set_import("ANativeWindow_getWidth",(void*)my_aw_getWidth);
  re4_set_import("ANativeWindow_getHeight",(void*)my_aw_getHeight);
  re4_set_import("ANativeWindow_acquire",(void*)my_aw_acquire);
  re4_set_import("ANativeWindow_release",(void*)my_aw_release);
  re4_set_import("mmap",(void*)my_mmap);
  re4_set_import("mprotect",(void*)my_mprotect);
  re4_set_import("sysconf",(void*)my_sysconf);
  re4_set_import("__sF",(void*)g_sf);
  re4_set_import("fprintf",(void*)my_fprintf);
  re4_set_import("vfprintf",(void*)my_vfprintf);
  re4_set_import("fputs",(void*)my_fputs);
  re4_set_import("fwrite",(void*)my_fwrite);
  re4_set_import("fputc",(void*)my_fputc);
  re4_set_import("fflush",(void*)my_fflush);
  so_resolve(dynlib_functions,dynlib_numfunctions,0); so_finalize();
  so_execute_init_array();
  fprintf(stderr,"[A] engine init OK (372 ctors)\n");
  g_unity_base=(uintptr_t)text_virtbase; g_m_unity=so_save();
  /* DIAG: a UnityMain trava em WaitForJobGroup (libunity+0x3268e0) no 1o nativeRender -- os jobs
     nunca completam (sem workers / inline-exec callback NULL). Hook p/ retornar imediato e ver
     se a engine progride (jobs nao-criticos) ou crasha (criticos). Gated. */
  if(getenv("RE4_SKIPJOBWAIT")){ uintptr_t ha=g_unity_base+0x3268e0;
    /* libunity ja foi finalizada (text r-x) -> precisa mprotect RWX antes de escrever o hook */
    uintptr_t pg=ha&~0xfffUL; mprotect((void*)pg,0x2000,PROT_READ|PROT_WRITE|PROT_EXEC);
    hook_arm64(ha,(uintptr_t)jobwait_stub); so_flush_caches(); fprintf(stderr,"[HOOK] WaitForJobGroup @unity+0x3268e0 -> return 0\n"); }
  { size_t msz=24*1024*1024; void *mh=mmap(NULL,msz,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(mh!=MAP_FAILED && so_load("libmono.so",mh,msz)>=0){ so_relocate(); so_resolve(dynlib_functions,dynlib_numfunctions,0);
      { uintptr_t a;
        /* Os hooks de mono_exception_from_name* interceptam a CRIACAO de exceptions. O Mono
           PRE-CRIA o singleton de OutOfMemoryException no init (em [domain+28]) -> nosso hook
           matava o processo achando que era um throw. Gated em RE4_HOOKEXC (so p/ debug). */
        if(getenv("RE4_HOOKEXC")){
          a=so_find_addr_safe("mono_exception_from_name_msg"); if(a)hook_arm64(a,(uintptr_t)hook_exc_msg);
          a=so_find_addr_safe("mono_exception_from_name_two_strings"); if(a)hook_arm64(a,(uintptr_t)hook_exc_two);
          a=so_find_addr_safe("mono_exception_from_name"); if(a)hook_arm64(a,(uintptr_t)hook_exc_name);
        }
        a=so_find_addr_safe("mono_valloc"); if(a)hook_arm64(a,(uintptr_t)my_mono_valloc);
        /* o C# do jogo chama GC.Collect (mono_gc_collect) no init -> stop-the-world trava (suspensao
           de thread por sinal nao sincroniza no bionic/glibc). No-op pula o GC explicito. Gated. */
        if(getenv("RE4_NOGCCOLLECT")){ a=so_find_addr_safe("mono_gc_collect"); if(a){hook_arm64(a,(uintptr_t)jobwait_stub); fprintf(stderr,"[HOOK] mono_gc_collect -> no-op\n");}
          a=so_find_addr_safe("mono_gc_collect_a_little"); if(a)hook_arm64(a,(uintptr_t)jobwait_stub); }
        a=so_find_addr_safe("mono_pagesize"); if(a){hook_arm64(a,(uintptr_t)my_mono_pagesize); fprintf(stderr,"[HOOK] mono_pagesize -> 4096\n");}
        g_mono_string_to_utf8_fn=(void*)so_find_addr_safe("mono_string_to_utf8");
        g_mono_free_fn=(void*)so_find_addr_safe("mono_free");
        a=so_find_addr_safe("mono_add_internal_call");
        if(a){
          unsigned char*t4=(unsigned char*)mmap(0,32,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
          if(t4!=MAP_FAILED){
            memcpy(t4,(void*)a,8);
            *(uint32_t*)(t4+8)=0xe51ff004u;
            *(uint32_t*)(t4+12)=(uint32_t)(a+8);
            __builtin___clear_cache((char*)t4,(char*)t4+16);
            g_orig_mono_add_internal_call=(void(*)(const char*,const void*))t4;
            hook_arm64(a,(uintptr_t)my_mono_add_internal_call);
            fprintf(stderr,"[HOOK] mono_add_internal_call -> icall filter\n");
          }
        }
        { uintptr_t ps=so_find_addr_safe("mono_pagesize"); uintptr_t base=ps-0x29d7e4; uintptr_t gt=base+0x1a6a8;
          g_mono_base=base;
          g_grd_fn=(void*)so_find_addr_safe("mono_get_root_domain"); g_dget_fn=(void*)so_find_addr_safe("mono_domain_get"); g_dset_fn=(void*)so_find_addr_safe("mono_domain_set"); g_jatt_fn=(void*)so_find_addr_safe("mono_jit_thread_attach");
          unsigned char*tr=(unsigned char*)mmap(0,32,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
          if(tr!=MAP_FAILED){ memcpy(tr,(void*)gt,8); *(uint32_t*)(tr+8)=0xe51ff004u; *(uint32_t*)(tr+12)=(uint32_t)(gt+8);
            __builtin___clear_cache((char*)tr,(char*)tr+16); g_orig_jitgetter=(void*(*)(void))tr;
            hook_arm64(gt,(uintptr_t)my_jit_tls_getter); fprintf(stderr,"[HOOK] jit_tls getter @0x1a6a8 base=%p tramp=%p\n",(void*)base,(void*)tr); }
          { uintptr_t gm=base+0x2bed14; unsigned char*t2=(unsigned char*)mmap(0,32,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
            if(t2!=MAP_FAILED){ memcpy(t2,(void*)gm,8); *(uint32_t*)(t2+8)=0xe51ff004u; *(uint32_t*)(t2+12)=(uint32_t)(gm+8);
              __builtin___clear_cache((char*)t2,(char*)t2+16); g_orig_getmem=(void*(*)(unsigned))t2;
              hook_arm64(gm,(uintptr_t)my_getmem); fprintf(stderr,"[HOOK] GET_MEM @0x2bed14\n"); } }
          { uintptr_t jv=base+0xbd5d0; unsigned char*t3=(unsigned char*)mmap(0,32,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
            if(t3!=MAP_FAILED){ memcpy(t3,(void*)jv,8); *(uint32_t*)(t3+8)=0xe51ff004u; *(uint32_t*)(t3+12)=(uint32_t)(jv+8);
              __builtin___clear_cache((char*)t3,(char*)t3+16); g_orig_jitinitver=(void*(*)(const char*,const char*))t3;
              hook_arm64(jv,(uintptr_t)my_jit_init_version); fprintf(stderr,"[HOOK] jit_init_version @0xbd5d0\n"); } }
          /* 0x2bcfdc NAO e assert: e o INSTALADOR do print/log handler (instala a func 0x13dec0
             -> mono_trace). Hookar com no-op DEIXAVA o global do handler NULL -> o runtime
             chamava o handler NULL = o "NULL-call no init" (pc=0). REMOVIDO: deixa instalar. */
          if(getenv("RE4_HOOKASSERT")){ hook_arm64(base+0x2bcfdc,(uintptr_t)my_assert_handler); fprintf(stderr,"[HOOK] assert handler @0x2bcfdc (LEGADO/gated)\n"); }
          else fprintf(stderr,"[NOHOOK] 0x2bcfdc deixado intacto (instalador de print-handler)\n"); }
        so_flush_caches(); }
      so_finalize(); so_execute_init_array(); g_m_mono=so_save(); fprintf(stderr,"[MONO] libmono carregado+init OK\n"); }
    else fprintf(stderr,"[MONO] FALHOU carregar libmono\n"); }
  so_use(g_m_unity);
  void *vm=NULL,*env=NULL; jni_shim_init(&vm,&env);
  jni_shim_set_package((pkg&&pkg[0])?pkg:"com.WS.RE4", obb_version);
  fprintf(stderr,"[JNI] package=%s obb=%d gamedir=%s\n",(pkg&&pkg[0])?pkg:"com.WS.RE4",obb_version,re4_gamedir());
  uintptr_t onload=so_find_addr_safe("JNI_OnLoad");
  jint ver=((JNI_OnLoad_t)onload)(vm,NULL);
  fprintf(stderr,"[B] JNI_OnLoad=0x%x\n",ver);
  static long t=0xA1, surf=0x5F, ctx=0xC0;
  /* janela GLES2 do device (egl_shim do core, provado) */
  if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_AUDIO|SDL_INIT_GAMECONTROLLER)!=0) fprintf(stderr,"SDL_Init: %s\n",SDL_GetError());
  egl_shim_create_window();
  fprintf(stderr,"[C] janela SDL/GLES2 criada\n");
  void *fn;
  if((fn=N("initJni"))) ((void(*)(void*,void*,void*))fn)(env,&t,&ctx);
  fprintf(stderr,"[D] initJni OK\n");
  if((fn=N("nativeRecreateGfxState"))) ((void(*)(void*,void*,int,void*))fn)(env,&t,0,&surf);
  if((fn=N("nativeResume"))) ((void(*)(void*,void*))fn)(env,&t);
  if((fn=N("nativeSendSurfaceChangedEvent"))) ((void(*)(void*,void*))fn)(env,&t);
  if((fn=N("nativeFocusChanged"))) ((void(*)(void*,void*,int))fn)(env,&t,1);
  fprintf(stderr,"[E] lifecycle OK -> nativeRender loop\n");
  /* ATTACH da thread ao Mono -> seta o jit_tls (senao assert mini.c:2215) */
  { so_module*c=so_save(); so_use(g_m_mono);
    void*(*grd)(void)=(void*)so_find_addr_safe("mono_get_root_domain");
    void*(*dget)(void)=(void*)so_find_addr_safe("mono_domain_get");
    void (*dset)(void*,int)=(void*)so_find_addr_safe("mono_domain_set");
    void*(*att)(void*)=(void*)so_find_addr_safe("mono_thread_attach");
    void*(*jatt)(void*)=(void*)so_find_addr_safe("mono_jit_thread_attach");
    so_use(c); free(c);
    void *d = grd?grd():NULL;
    if(!d && dget) d=dget();
    if(d && dset) dset(d,0);
    fprintf(stderr,"[MONO] root_domain=%p att=%p jatt=%p dget=%p dset=%p\n",d,(void*)att,(void*)jatt,(void*)dget,(void*)dset);
    if(d && jatt){ void*th=jatt(d); fprintf(stderr,"[MONO] jit_thread_attach -> %p\n",th); }
    else if(d && att){ void*th=att(d); fprintf(stderr,"[MONO] thread_attach -> %p\n",th); }
  }
  /* SOM: dispara a thread que bombeia fmodProcess (FMOD AudioTrack -> SDL). Desliga c/ RE4_NOAUDIO=1 */
  if(!getenv("RE4_NOAUDIO")){
    g_fmod_env = env; g_fmod_run = 1;
    pthread_t at; if(pthread_create(&at, NULL, fmod_audio_thread, NULL)==0) pthread_detach(at);
    fprintf(stderr,"[AUDIO] fmod_audio_thread iniciada\n");
  }
  void *render=N("nativeRender");
  void *inject=N("nativeInjectEvent");
  int maxframes = getenv("RE4_MAXFRAMES")?atoi(getenv("RE4_MAXFRAMES")):1200;
  if(maxframes<=0) maxframes=1<<30;
  for(int f=0; render && f<maxframes; f++){
    g_re4_frame=f;
    re4_pump_sdl_input(env, &t, inject, f);
    re4_autotap(env, &t, inject, f);
    re4_keyseq(env, &t, inject, f);
    ((unsigned char(*)(void*,void*))render)(env,&t);
    re4_enable_menu_nav(f);   /* habilita Navigation nos botoes do menu (gotokey) */
    re4_menu_drive(f);        /* gotokey: forca selecao por nome (/tmp/re4sel) - debug */
    re4_menu_nav(f);          /* NAVEGACAO COMPLETA: dpad move cursor, A=onClick, B=Back */
    re4_log_selected(f);      /* DIAG: nome do botao selecionado (apos nav = final) */
    re4_probe_frame_pixels(f);
    re4_dump_framebuffers_if_needed(f);
    re4_force_fbo1_blit_if_needed(f);
    if(!getenv("RE4_SKIP_FRAME_PRESENT")) egl_shim_force_present("frame-end");
    opensles_shim_pump_callbacks(); /* alimenta o audio (OpenSL->SDL2) */
    re4_log_and_reset_fbo_stats(f);
    if(f<5||f%100==0) fprintf(stderr,"[render %d]\n",f);
  }
  fprintf(stderr,"=== render loop terminou ===\n");
  return 0;
}
