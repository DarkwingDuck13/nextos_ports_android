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
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include "so_util.h"
#include "imports.h"
#include "android_shim.h"
#include "jni_shim.h"
#include "opensles_shim.h"
/* RAIZ do "invalid CIL": glibc fstatat64 preenche struct stat64 layout GLIBC, mas Unity
   (bionic) le st_size no offset BIONIC -> tamanho errado (32KB) -> le so 32KB do .dll.
   Fix: syscall CRU -> o kernel preenche o layout stat64 do kernel (== bionic). */
static int my_fstatat64(int dfd,const char*p,void*b,int fl){ return (int)syscall(__NR_fstatat64,dfd,p,b,fl); }
static int my_fstat64(int fd,void*b){ return (int)syscall(__NR_fstat64,fd,b); }
static int my_stat64(const char*p,void*b){ return (int)syscall(__NR_stat64,p,b); }
static int my_lstat64(const char*p,void*b){ return (int)syscall(__NR_lstat64,p,b); }
static uintptr_t g_mono_base=0, g_unity_base=0;
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
static void my_mono_add_internal_call(const char *name, const void *method){
  const void *resolved = method;
  if(name){
    if((strstr(name, "Handheld") || strstr(name, "Movie") || strstr(name, "Video")) && method)
      fprintf(stderr, "[ICALL] %s -> %p\n", name, method);
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
static void (*r_glDrawArrays)(unsigned,int,int);
static void (*r_glDrawElements)(unsigned,int,unsigned,const void*);
static void (*r_glUseProgram)(unsigned);
static void (*r_glActiveTexture)(unsigned);
static void (*r_glBindTexture)(unsigned,unsigned);
static void (*r_glViewport)(int,int,int,int);
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
static unsigned g_gl_bound_fbo=0;
static int g_re4_frame=-1;
static unsigned g_gl_current_program=0;
static unsigned g_gl_active_texture=0x84c0; /* GL_TEXTURE0 */
static unsigned g_gl_bound_tex2d[4]={0,0,0,0};
static unsigned g_gl_bound_texext[4]={0,0,0,0};
static int g_gl_viewport[4]={0,0,0,0};
static int g_gl_blend_enabled=0;
static unsigned g_gl_blend_src_rgb=1, g_gl_blend_dst_rgb=0, g_gl_blend_src_alpha=1, g_gl_blend_dst_alpha=0;
static unsigned char g_gl_color_mask[4]={1,1,1,1};
static unsigned g_fbo_color_tex[8]={0};
static int g_last_composite_log_frame=-1;
static unsigned g_re4_blit_program=0;
static int g_re4_blit_a_pos=-1, g_re4_blit_a_uv=-1, g_re4_blit_u_tex=-1;
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
static void re4_log_composite_state(const char *kind, unsigned mode, int count){
  int idx=re4_texunit_index(g_gl_active_texture);
  if(g_gl_bound_fbo!=0 || !re4_interesting_composite_frame(g_re4_frame) || g_last_composite_log_frame==g_re4_frame)
    return;
  g_last_composite_log_frame=g_re4_frame;
  fprintf(stderr,
          "[COMPOSITE] f=%d via=%s mode=0x%x count=%d prog=%u vp=%d,%d %dx%d blend=%d func=%u/%u/%u/%u mask=%u%u%u%u act=%u tex2d={%u,%u,%u,%u} texext={%u,%u,%u,%u} act2d=%u actext=%u fbo1tex=%u fbo2tex=%u\n",
          g_re4_frame, kind?kind:"?", mode, count, g_gl_current_program,
          g_gl_viewport[0], g_gl_viewport[1], g_gl_viewport[2], g_gl_viewport[3],
          g_gl_blend_enabled,
          g_gl_blend_src_rgb, g_gl_blend_dst_rgb, g_gl_blend_src_alpha, g_gl_blend_dst_alpha,
          g_gl_color_mask[0], g_gl_color_mask[1], g_gl_color_mask[2], g_gl_color_mask[3],
          g_gl_active_texture,
          g_gl_bound_tex2d[0], g_gl_bound_tex2d[1], g_gl_bound_tex2d[2], g_gl_bound_tex2d[3],
          g_gl_bound_texext[0], g_gl_bound_texext[1], g_gl_bound_texext[2], g_gl_bound_texext[3],
          idx>=0?g_gl_bound_tex2d[idx]:0, idx>=0?g_gl_bound_texext[idx]:0,
          g_fbo_color_tex[1], g_fbo_color_tex[2]);
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
      "attribute vec2 a_uv;\n"
      "varying vec2 v_uv;\n"
      "void main(void){\n"
      "  v_uv = a_uv;\n"
      "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
      "}\n";
  static const char *fs_src =
      "precision mediump float;\n"
      "varying vec2 v_uv;\n"
      "uniform sampler2D u_tex;\n"
      "void main(void){\n"
      "  gl_FragColor = texture2D(u_tex, v_uv);\n"
      "}\n";
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
	  if(getenv("RE4_GLDIAG") && (n==0x1F00||n==0x1F01||n==0x1F02||n==0x8B8C)) fprintf(stderr,"[GLSTR] 0x%x = %s\n",n,s?(const char*)s:"(null)");
  return s; }
static int re4_gl_rt_max(void){
  const char *v=getenv("RE4_GLRT_MAX"); char *e=NULL; long n;
  if(!v||!v[0]) return 1024;
  n=strtol(v,&e,10);
  if(!e||*e) return 1024;
  if(n<256) n=256;
  if(n>4096) n=4096;
  return (int)n;
}
static void my_glTexImage2D(unsigned target,int level,int internalformat,int width,int height,int border,unsigned format,unsigned type,const void *pixels){
  if(!r_glTexImage2D) r_glTexImage2D=dlsym(RTLD_DEFAULT,"glTexImage2D");
  if(r_glTexImage2D){
    int maxdim=re4_gl_rt_max();
    if(level==0 && pixels==NULL && (width>maxdim || height>maxdim)){
      int ow=width, oh=height; static int lg=0;
      while(width>maxdim || height>maxdim){ width=(width+1)/2; height=(height+1)/2; }
      if(lg++<24) fprintf(stderr,"[GLRT] glTexImage2D %dx%d -> %dx%d fmt=0x%x ifmt=0x%x\n",ow,oh,width,height,format,internalformat);
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
static void my_glClear(unsigned mask){
  if(!r_glClear) r_glClear=dlsym(RTLD_DEFAULT,"glClear");
  if(r_glClear){
    re4_count_fbo_bucket(g_gl_bound_fbo, 1);
    static int lg=0;
    if(lg++<48) fprintf(stderr,"[GLDRAW] clear fbo=%u mask=0x%x\n",g_gl_bound_fbo,mask);
    r_glClear(mask);
  }
}
static void my_glDrawArrays(unsigned mode,int first,int count){
  if(!r_glDrawArrays) r_glDrawArrays=dlsym(RTLD_DEFAULT,"glDrawArrays");
  if(r_glDrawArrays){
    re4_count_fbo_bucket(g_gl_bound_fbo, 0);
    re4_log_composite_state("arrays", mode, count);
    static int lg=0;
    if(lg++<64) fprintf(stderr,"[GLDRAW] arrays fbo=%u mode=0x%x first=%d count=%d\n",g_gl_bound_fbo,mode,first,count);
    r_glDrawArrays(mode,first,count);
    if(!g_gl_bound_fbo) egl_shim_force_present("glDrawArrays");
  }
}
static void my_glDrawElements(unsigned mode,int count,unsigned type,const void *indices){
  if(!r_glDrawElements) r_glDrawElements=dlsym(RTLD_DEFAULT,"glDrawElements");
  if(r_glDrawElements){
    re4_count_fbo_bucket(g_gl_bound_fbo, 0);
    re4_log_composite_state("elems", mode, count);
    static int lg=0;
    if(lg++<64) fprintf(stderr,"[GLDRAW] elems fbo=%u mode=0x%x count=%d type=0x%x idx=%p\n",g_gl_bound_fbo,mode,count,type,indices);
    r_glDrawElements(mode,count,type,indices);
    if(!g_gl_bound_fbo) egl_shim_force_present("glDrawElements");
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
  if(r_glViewport) r_glViewport(x,y,width,height);
}
static void my_glEnable(unsigned cap){
  if(!r_glEnable) r_glEnable=dlsym(RTLD_DEFAULT,"glEnable");
  if(cap==0x0be2) g_gl_blend_enabled=1; /* GL_BLEND */
  if(r_glEnable) r_glEnable(cap);
}
static void my_glDisable(unsigned cap){
  if(!r_glDisable) r_glDisable=dlsym(RTLD_DEFAULT,"glDisable");
  if(cap==0x0be2) g_gl_blend_enabled=0; /* GL_BLEND */
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
static long my_sysconf(int name){
  long ps=4096;
  switch(name){
    case 39: case 40: /* bionic _SC_PAGE_SIZE / _SC_PAGESIZE */
      fprintf(stderr,"[SYSCONF] bionic PAGESIZE(%d) -> 4096\n",name); return 4096;
    case 6:  /* bionic _SC_CLK_TCK */ return 100;
    case 96: case 97: /* bionic _SC_NPROCESSORS_CONF / _ONLN */
      /* 2 CPUs -> Unity cria 1 job worker que PROCESSA os jobs do WaitForJobGroup (com 1 core
         dava 0 workers e o WaitForJobGroup travava sem ninguem rodar os jobs inline). */
      fprintf(stderr,"[SYSCONF] bionic NPROC(%d) -> 2\n",name); return 2;
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
/* loga open/fopen -> acha a fonte de memoria (/proc/meminfo etc) */
static FILE* my_fopen(const char*p,const char*m){ if(p&&(strstr(p,"proc")||strstr(p,"mem")||strstr(p,"sys"))) fprintf(stderr,"[FOPEN] %s\n",p);
  if(p&&!strcmp(p,"/proc/meminfo")){ fprintf(stderr,"[FOPEN] meminfo -> fake 512MB\n");
    FILE*t=tmpfile(); if(t){ fputs("MemTotal:      524288 kB\nMemFree:       262144 kB\nMemAvailable:  262144 kB\n",t); rewind(t); return t; } }
  /* core count vem daqui (/sys/.../possible|present) -> Unity dimensiona job workers. Forcamos 1
     core ("0") -> jobs INLINE, sem workers, sem WaitForJobGroup deadlock. */
  if(p&&(!strcmp(p,"/sys/devices/system/cpu/possible")||!strcmp(p,"/sys/devices/system/cpu/present")||!strcmp(p,"/sys/devices/system/cpu/online"))){
    fprintf(stderr,"[FOPEN] %s -> fake 2 cores\n",p); FILE*t=tmpfile(); if(t){ fputs("0-1\n",t); rewind(t); return t; } }
  return fopen(p,m); }
static int my_open(const char*p,int fl,...){ if(p&&(strstr(p,"proc")||strstr(p,"mem"))) fprintf(stderr,"[OPEN] %s\n",p);
  /* /proc/cpuinfo: Unity conta cores p/ dimensionar o job worker pool. Forcamos 1 core (1 entrada
     "processor") -> jobs rodam INLINE, sem workers, sem WaitForJobGroup deadlock. */
  if(p&&!strcmp(p,"/proc/cpuinfo")){ FILE*t=tmpfile();
    if(t){ fputs("processor\t: 0\nmodel name\t: ARMv7 Processor rev 1 (v7l)\nFeatures\t: half thumb fastmult vfp edsp neon vfpv3\nCPU implementer\t: 0x41\nCPU architecture: 7\n\nprocessor\t: 1\nmodel name\t: ARMv7 Processor rev 1 (v7l)\nFeatures\t: half thumb fastmult vfp edsp neon vfpv3\nCPU implementer\t: 0x41\nCPU architecture: 7\n\n",t); fflush(t); int fd=dup(fileno(t)); fclose(t); lseek(fd,0,SEEK_SET); fprintf(stderr,"[OPEN] cpuinfo -> fake 2 cores (fd=%d)\n",fd); return fd; } }
  if(p&&(!strcmp(p,"/sys/devices/system/cpu/possible")||!strcmp(p,"/sys/devices/system/cpu/present")||!strcmp(p,"/sys/devices/system/cpu/online"))){
    FILE*t=tmpfile(); if(t){ fputs("0-1\n",t); fflush(t); int fd=dup(fileno(t)); fclose(t); lseek(fd,0,SEEK_SET); fprintf(stderr,"[OPEN] %s -> fake 2 cores\n",p); return fd; } }
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
  g_hk_inject.deviceId = 0;
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
static void re4_pump_sdl_input(void *env, void *thiz, void *inject, int frame){
  FakeInputEvent ev;
  android_shim_pump_sdl_events();
  while (android_shim_pop_input_event(&ev)) {
    if (ev.type == AINPUT_EVENT_TYPE_KEY) {
      re4_inject_key_event(env, thiz, inject, ev.action, ev.keycode, 0, frame, "SDL");
      continue;
    }
    static int motionlog = 0;
    if (motionlog++ < 16) {
      fprintf(stderr,
              "[PAD] motion src=0x%x act=%d hat=(%.2f,%.2f) stick=(%.2f,%.2f,%.2f,%.2f) f=%d\n",
              ev.source, ev.action,
              ev.axes[AMOTION_EVENT_AXIS_HAT_X], ev.axes[AMOTION_EVENT_AXIS_HAT_Y],
              ev.axes[AMOTION_EVENT_AXIS_X], ev.axes[AMOTION_EVENT_AXIS_Y],
              ev.axes[AMOTION_EVENT_AXIS_Z], ev.axes[AMOTION_EVENT_AXIS_RZ], frame);
    }
  }
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
  void *render=N("nativeRender");
  void *inject=N("nativeInjectEvent");
  for(int f=0; render && f<1200; f++){
    g_re4_frame=f;
    re4_pump_sdl_input(env, &t, inject, f);
    re4_autotap(env, &t, inject, f);
    ((unsigned char(*)(void*,void*))render)(env,&t);
    re4_probe_frame_pixels(f);
    re4_force_fbo1_blit_if_needed(f);
    if(!getenv("RE4_SKIP_FRAME_PRESENT")) egl_shim_force_present("frame-end");
    opensles_shim_pump_callbacks(); /* alimenta o audio (OpenSL->SDL2) */
    re4_log_and_reset_fbo_stats(f);
    if(f<5||f%100==0) fprintf(stderr,"[render %d]\n",f);
  }
  fprintf(stderr,"=== render loop terminou ===\n");
  return 0;
}
