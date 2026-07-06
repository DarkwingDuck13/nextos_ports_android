/* softfp_shim.c -- ponte de ABI float (armv7).
   O libShantaeCurse_android.so é SOFTFP (base AAPCS, Tag_ABI_VFP_args ausente):
   double/float passam em registradores INTEIROS (r0:r1, r0..r3). O glibc
   libm/libc E o libGLESv2 do device são HARDFP (Tag_ABI_VFP_args=VFP): float/
   double em d0..d7/s0..s3. Chamar essas libs direto do código softfp do jogo
   passa os args nos registradores errados -> lixo (ex: glClearColor recebeu
   (0,1280,0,720) = dims em vez de cor -> clear errado -> tela preta).

   Fix: wrappers declarados pcs("aapcs") (softfp) -> recebem como o jogo manda e
   chamam a função real (hardfp); o GCC faz a tradução de registradores. Roteados
   via softfp_resolve() ANTES do dlsym(RTLD_DEFAULT) no so_resolve. */
#include <dlfcn.h>
#include <execinfo.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "so_util.h"

#define SF __attribute__((pcs("aapcs")))

/* 1 arg double->double */
#define W1D(n)  SF double sf_##n(double x){ return n(x); }
/* 1 arg float->float */
#define W1F(n)  SF float  sf_##n(float x){ return n(x); }
/* 2 arg double,double->double */
#define W2D(n)  SF double sf_##n(double a,double b){ return n(a,b); }
/* 2 arg float,float->float */
#define W2F(n)  SF float  sf_##n(float a,float b){ return n(a,b); }

W1D(acos) W1D(asin) W1D(atan) W1D(cos) W1D(sin) W1D(tan)
W1D(cosh) W1D(sinh) W1D(tanh)
W1D(exp) W1D(exp2) W1D(log) W1D(log10) W1D(sqrt)
W1D(ceil) W1D(floor) W1D(round) W1D(trunc) W1D(rint)
W1F(acosf) W1F(asinf) W1F(atanf) W1F(cosf) W1F(sinf) W1F(tanf)
W1F(expf) W1F(logf) W1F(sqrtf) W1F(fabsf) W1F(ceilf) W1F(floorf) W1F(roundf) W1F(truncf)
W2D(atan2) W2D(fmod) W2D(pow) W2D(remainder)
W2F(atan2f) W2F(fmodf) W2F(powf)

SF double sf_modf(double x,double *iptr){ return modf(x,iptr); }
SF float  sf_modff(float x,float *iptr){ return modff(x,iptr); }
SF double sf_frexp(double x,int *e){ return frexp(x,e); }
SF double sf_ldexp(double x,int e){ return ldexp(x,e); }
SF double sf_strtod(const char *s,char **end){ return strtod(s,end); }

/* ---- GL GLESv2: o jogo e softfp; o driver no device e hardfp. ---- */
typedef unsigned int GLenum;
typedef unsigned int GLbitfield;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef intptr_t GLintptr;
typedef intptr_t GLsizeiptr;
typedef unsigned char GLboolean;
typedef float GLfloat;
typedef char GLchar;

static int gl_trace_enabled(void) {
  static int enabled = -1;
  if (enabled < 0)
    enabled = getenv("TASM2_GL_DEBUG") ? 1 : 0;
  return enabled;
}

static int gl_dense_enabled(void) {
  static int enabled = -1;
  if (enabled < 0)
    enabled = getenv("TASM2_GL_DENSE") ? 1 : 0;
  return enabled;
}

#define GL_SHOULD_LOG(n) \
  (gl_dense_enabled() ? ((n) <= 240 || (((n) % 60) == 0)) : \
                        ((n) <= 24 || (((n) % 300) == 0)))

#define GL_TRACE_N(tag, n, fmt, ...) do {                                 \
  if (gl_trace_enabled() && GL_SHOULD_LOG(n))                              \
    fprintf(stderr, "[GL] %s#%d " fmt "\n", tag, n, ##__VA_ARGS__);      \
} while (0)

#define GL_TRACE(tag, fmt, ...) do {                                      \
  if (gl_trace_enabled()) {                                                \
    static int _n;                                                         \
    _n++;                                                                 \
    GL_TRACE_N(tag, _n, fmt, ##__VA_ARGS__);                               \
  }                                                                       \
} while (0)

static const char *gl_enum_name(GLenum e);
static void log_current_pixel(const char *tag, int n);
static void log_draw_state(const char *tag, int n);

static GLuint g_current_program;
static GLenum g_active_texture = 0x84C0; /* GL_TEXTURE0 */
static GLuint g_bound_tex2d[16];

static int active_texture_index(void) {
  if (g_active_texture >= 0x84C0 && g_active_texture < 0x84C0 + 16)
    return (int)(g_active_texture - 0x84C0);
  return 0;
}

static void log_gl_caller(const char *tag, int n, uintptr_t ra) {
  if (!gl_trace_enabled() || !GL_SHOULD_LOG(n))
    return;
  if (text_base && ra >= (uintptr_t)text_base &&
      ra < (uintptr_t)text_base + text_size) {
    fprintf(stderr, "[GLCALL] %s#%d caller=libtasm2.so+0x%lx\n", tag, n,
            (unsigned long)(ra - (uintptr_t)text_base));
  } else {
    fprintf(stderr, "[GLCALL] %s#%d caller=%p\n", tag, n, (void *)ra);
  }
}

static void log_gl_return_chain_once(const char *tag) {
  if (!getenv("TASM2_GL_BACKTRACE"))
    return;
  static int done;
  if (done)
    return;
  done = 1;

  uintptr_t a = (uintptr_t)__builtin_return_address(0);
  if (text_base && a >= (uintptr_t)text_base &&
      a < (uintptr_t)text_base + text_size) {
    fprintf(stderr, "[GLBT] %s caller=libtasm2.so+0x%lx\n", tag,
            (unsigned long)(a - (uintptr_t)text_base));
  } else {
    fprintf(stderr, "[GLBT] %s caller=%p\n", tag, (void *)a);
  }
}

SF void sf_glClearColor(float r,float g,float b,float a){
  static void(*real)(float,float,float,float)=0;
  if(!real) real=(void(*)(float,float,float,float))dlsym(RTLD_DEFAULT,"glClearColor");
  static int n;
  n++;
  GL_TRACE_N("glClearColor", n, "%.3f %.3f %.3f %.3f", r, g, b, a);
  log_gl_caller("glClearColor", n, (uintptr_t)__builtin_return_address(0));
  if (r > 0.49f && r < 0.51f && g > 0.49f && g < 0.51f && b > 0.99f)
    log_gl_return_chain_once("purple-clear");
  if(real) real(r,g,b,a);
}
SF void sf_glBlendColor(GLfloat r,GLfloat g,GLfloat b,GLfloat a){
  static void(*real)(GLfloat,GLfloat,GLfloat,GLfloat)=0;
  if(!real) real=(void(*)(GLfloat,GLfloat,GLfloat,GLfloat))dlsym(RTLD_DEFAULT,"glBlendColor");
  if(real) real(r,g,b,a);
}
SF void sf_glClearDepthf(GLfloat d){
  static void(*real)(GLfloat)=0;
  if(!real) real=(void(*)(GLfloat))dlsym(RTLD_DEFAULT,"glClearDepthf");
  if(real) real(d);
}
SF void sf_glDepthRangef(GLfloat n,GLfloat f){
  static void(*real)(GLfloat,GLfloat)=0;
  if(!real) real=(void(*)(GLfloat,GLfloat))dlsym(RTLD_DEFAULT,"glDepthRangef");
  if(real) real(n,f);
}
SF void sf_glLineWidth(GLfloat w){
  static void(*real)(GLfloat)=0;
  if(!real) real=(void(*)(GLfloat))dlsym(RTLD_DEFAULT,"glLineWidth");
  if(real) real(w);
}
SF void sf_glPolygonOffset(GLfloat factor,GLfloat units){
  static void(*real)(GLfloat,GLfloat)=0;
  if(!real) real=(void(*)(GLfloat,GLfloat))dlsym(RTLD_DEFAULT,"glPolygonOffset");
  if(real) real(factor,units);
}
SF void sf_glSampleCoverage(GLfloat value,GLboolean invert){
  static void(*real)(GLfloat,GLboolean)=0;
  if(!real) real=(void(*)(GLfloat,GLboolean))dlsym(RTLD_DEFAULT,"glSampleCoverage");
  if(real) real(value,invert);
}
SF void sf_glTexParameterf(GLenum target,GLenum pname,GLfloat param){
  static void(*real)(GLenum,GLenum,GLfloat)=0;
  if(!real) real=(void(*)(GLenum,GLenum,GLfloat))dlsym(RTLD_DEFAULT,"glTexParameterf");
  if(real) real(target,pname,param);
}
SF void sf_glUniform1f(GLint loc,GLfloat v0){
  static void(*real)(GLint,GLfloat)=0;
  if(!real) real=(void(*)(GLint,GLfloat))dlsym(RTLD_DEFAULT,"glUniform1f");
  if(real) real(loc,v0);
}
SF void sf_glVertexAttrib4f(GLuint index,GLfloat x,GLfloat y,GLfloat z,GLfloat w){
  static void(*real)(GLuint,GLfloat,GLfloat,GLfloat,GLfloat)=0;
  if(!real) real=(void(*)(GLuint,GLfloat,GLfloat,GLfloat,GLfloat))dlsym(RTLD_DEFAULT,"glVertexAttrib4f");
  if(real) real(index,x,y,z,w);
}

SF void sf_glViewport(GLint x,GLint y,GLsizei w,GLsizei h){
  static void(*real)(GLint,GLint,GLsizei,GLsizei)=0;
  if(!real) real=(void(*)(GLint,GLint,GLsizei,GLsizei))dlsym(RTLD_DEFAULT,"glViewport");
  GL_TRACE("glViewport", "%d,%d %dx%d", x, y, w, h);
  if(real) real(x,y,w,h);
}
SF void sf_glClear(GLbitfield mask){
  static void(*real)(GLbitfield)=0;
  if(!real) real=(void(*)(GLbitfield))dlsym(RTLD_DEFAULT,"glClear");
  static int n;
  n++;
  GL_TRACE_N("glClear", n, "mask=0x%x", mask);
  log_gl_caller("glClear", n, (uintptr_t)__builtin_return_address(0));
  if(real) real(mask);
  log_current_pixel("glClear", n);
}
SF void sf_glDrawArrays(GLenum mode,GLint first,GLsizei count){
  static void(*real)(GLenum,GLint,GLsizei)=0;
  if(!real) real=(void(*)(GLenum,GLint,GLsizei))dlsym(RTLD_DEFAULT,"glDrawArrays");
  static int n;
  n++;
  GL_TRACE_N("glDrawArrays", n, "mode=0x%x(%s) first=%d count=%d",
             mode, gl_enum_name(mode), first, count);
  log_draw_state("glDrawArrays", n);
  if(real) real(mode,first,count);
  log_current_pixel("glDrawArrays", n);
}
SF void sf_glDrawElements(GLenum mode,GLsizei count,GLenum type,const void *indices){
  static void(*real)(GLenum,GLsizei,GLenum,const void*)=0;
  if(!real) real=(void(*)(GLenum,GLsizei,GLenum,const void*))dlsym(RTLD_DEFAULT,"glDrawElements");
  static int n;
  n++;
  GL_TRACE_N("glDrawElements", n, "mode=0x%x(%s) count=%d type=0x%x(%s) indices=%p",
             mode, gl_enum_name(mode), count, type, gl_enum_name(type), indices);
  log_draw_state("glDrawElements", n);
  if(real) real(mode,count,type,indices);
  log_current_pixel("glDrawElements", n);
}
SF void sf_glBindFramebuffer(GLenum target,GLuint fb){
  static void(*real)(GLenum,GLuint)=0;
  if(!real) real=(void(*)(GLenum,GLuint))dlsym(RTLD_DEFAULT,"glBindFramebuffer");
  GL_TRACE("glBindFramebuffer", "target=0x%x(%s) fb=%u", target, gl_enum_name(target), fb);
  if(real) real(target,fb);
}
SF void sf_glUseProgram(GLuint program){
  static void(*real)(GLuint)=0;
  if(!real) real=(void(*)(GLuint))dlsym(RTLD_DEFAULT,"glUseProgram");
  g_current_program = program;
  GL_TRACE("glUseProgram", "%u", program);
  if(real) real(program);
}

SF void sf_glActiveTexture(GLenum texture){
  static void(*real)(GLenum)=0;
  if(!real) real=(void(*)(GLenum))dlsym(RTLD_DEFAULT,"glActiveTexture");
  g_active_texture = texture;
  GL_TRACE("glActiveTexture", "0x%x(%s)", texture, gl_enum_name(texture));
  if(real) real(texture);
}
SF void sf_glBindTexture(GLenum target,GLuint texture){
  static void(*real)(GLenum,GLuint)=0;
  if(!real) real=(void(*)(GLenum,GLuint))dlsym(RTLD_DEFAULT,"glBindTexture");
  if (target == 0x0DE1)
    g_bound_tex2d[active_texture_index()] = texture;
  GL_TRACE("glBindTexture", "target=0x%x(%s) tex=%u", target, gl_enum_name(target), texture);
  if(real) real(target,texture);
}
SF void sf_glTexParameteri(GLenum target,GLenum pname,GLint param){
  static void(*real)(GLenum,GLenum,GLint)=0;
  if(!real) real=(void(*)(GLenum,GLenum,GLint))dlsym(RTLD_DEFAULT,"glTexParameteri");
  GL_TRACE("glTexParameteri", "target=0x%x(%s) pname=0x%x(%s) param=0x%x(%s)",
           target, gl_enum_name(target), pname, gl_enum_name(pname),
           param, gl_enum_name((GLenum)param));
  if(real) real(target,pname,param);
}
SF void sf_glBindBuffer(GLenum target,GLuint buffer){
  static void(*real)(GLenum,GLuint)=0;
  if(!real) real=(void(*)(GLenum,GLuint))dlsym(RTLD_DEFAULT,"glBindBuffer");
  GL_TRACE("glBindBuffer", "target=0x%x(%s) buf=%u", target, gl_enum_name(target), buffer);
  if(real) real(target,buffer);
}
SF void sf_glBufferData(GLenum target,GLsizeiptr size,const void *data,GLenum usage){
  static void(*real)(GLenum,GLsizeiptr,const void*,GLenum)=0;
  if(!real) real=(void(*)(GLenum,GLsizeiptr,const void*,GLenum))dlsym(RTLD_DEFAULT,"glBufferData");
  GL_TRACE("glBufferData", "target=0x%x(%s) size=%ld data=%p usage=0x%x",
           target, gl_enum_name(target), (long)size, data, usage);
  if(real) real(target,size,data,usage);
}
SF void sf_glBufferSubData(GLenum target,GLintptr offset,GLsizeiptr size,const void *data){
  static void(*real)(GLenum,GLintptr,GLsizeiptr,const void*)=0;
  if(!real) real=(void(*)(GLenum,GLintptr,GLsizeiptr,const void*))dlsym(RTLD_DEFAULT,"glBufferSubData");
  GL_TRACE("glBufferSubData", "target=0x%x(%s) off=%ld size=%ld data=%p",
           target, gl_enum_name(target), (long)offset, (long)size, data);
  if(real) real(target,offset,size,data);
}
SF void sf_glEnable(GLenum cap){
  static void(*real)(GLenum)=0;
  if(!real) real=(void(*)(GLenum))dlsym(RTLD_DEFAULT,"glEnable");
  GL_TRACE("glEnable", "0x%x(%s)", cap, gl_enum_name(cap));
  if(real) real(cap);
}
SF void sf_glDisable(GLenum cap){
  static void(*real)(GLenum)=0;
  if(!real) real=(void(*)(GLenum))dlsym(RTLD_DEFAULT,"glDisable");
  GL_TRACE("glDisable", "0x%x(%s)", cap, gl_enum_name(cap));
  if(real) real(cap);
}
SF void sf_glBlendFunc(GLenum sfactor,GLenum dfactor){
  static void(*real)(GLenum,GLenum)=0;
  if(!real) real=(void(*)(GLenum,GLenum))dlsym(RTLD_DEFAULT,"glBlendFunc");
  GL_TRACE("glBlendFunc", "src=0x%x(%s) dst=0x%x(%s)",
           sfactor, gl_enum_name(sfactor), dfactor, gl_enum_name(dfactor));
  if(real) real(sfactor,dfactor);
}
SF void sf_glBlendFuncSeparate(GLenum srcRGB,GLenum dstRGB,GLenum srcAlpha,GLenum dstAlpha){
  static void(*real)(GLenum,GLenum,GLenum,GLenum)=0;
  if(!real) real=(void(*)(GLenum,GLenum,GLenum,GLenum))dlsym(RTLD_DEFAULT,"glBlendFuncSeparate");
  GL_TRACE("glBlendFuncSeparate", "rgb=0x%x(%s),0x%x(%s) a=0x%x(%s),0x%x(%s)",
           srcRGB, gl_enum_name(srcRGB), dstRGB, gl_enum_name(dstRGB),
           srcAlpha, gl_enum_name(srcAlpha), dstAlpha, gl_enum_name(dstAlpha));
  if(real) real(srcRGB,dstRGB,srcAlpha,dstAlpha);
}
SF void sf_glBlendEquation(GLenum mode){
  static void(*real)(GLenum)=0;
  if(!real) real=(void(*)(GLenum))dlsym(RTLD_DEFAULT,"glBlendEquation");
  GL_TRACE("glBlendEquation", "0x%x(%s)", mode, gl_enum_name(mode));
  if(real) real(mode);
}
SF void sf_glBlendEquationSeparate(GLenum modeRGB,GLenum modeAlpha){
  static void(*real)(GLenum,GLenum)=0;
  if(!real) real=(void(*)(GLenum,GLenum))dlsym(RTLD_DEFAULT,"glBlendEquationSeparate");
  GL_TRACE("glBlendEquationSeparate", "rgb=0x%x(%s) a=0x%x(%s)",
           modeRGB, gl_enum_name(modeRGB), modeAlpha, gl_enum_name(modeAlpha));
  if(real) real(modeRGB,modeAlpha);
}
SF void sf_glColorMask(GLboolean r,GLboolean g,GLboolean b,GLboolean a){
  static void(*real)(GLboolean,GLboolean,GLboolean,GLboolean)=0;
  if(!real) real=(void(*)(GLboolean,GLboolean,GLboolean,GLboolean))dlsym(RTLD_DEFAULT,"glColorMask");
  GL_TRACE("glColorMask", "%u,%u,%u,%u", r, g, b, a);
  if(real) real(r,g,b,a);
}
SF void sf_glDepthMask(GLboolean flag){
  static void(*real)(GLboolean)=0;
  if(!real) real=(void(*)(GLboolean))dlsym(RTLD_DEFAULT,"glDepthMask");
  GL_TRACE("glDepthMask", "%u", flag);
  if(real) real(flag);
}
SF void sf_glDepthFunc(GLenum func){
  static void(*real)(GLenum)=0;
  if(!real) real=(void(*)(GLenum))dlsym(RTLD_DEFAULT,"glDepthFunc");
  GL_TRACE("glDepthFunc", "0x%x(%s)", func, gl_enum_name(func));
  if(real) real(func);
}
SF void sf_glScissor(GLint x,GLint y,GLsizei width,GLsizei height){
  static void(*real)(GLint,GLint,GLsizei,GLsizei)=0;
  if(!real) real=(void(*)(GLint,GLint,GLsizei,GLsizei))dlsym(RTLD_DEFAULT,"glScissor");
  GL_TRACE("glScissor", "%d,%d %dx%d", x, y, width, height);
  if(real) real(x,y,width,height);
}
SF void sf_glStencilFunc(GLenum func,GLint ref,GLuint mask){
  static void(*real)(GLenum,GLint,GLuint)=0;
  if(!real) real=(void(*)(GLenum,GLint,GLuint))dlsym(RTLD_DEFAULT,"glStencilFunc");
  GL_TRACE("glStencilFunc", "func=0x%x(%s) ref=%d mask=0x%x", func, gl_enum_name(func), ref, mask);
  if(real) real(func,ref,mask);
}
SF void sf_glStencilOp(GLenum fail,GLenum zfail,GLenum zpass){
  static void(*real)(GLenum,GLenum,GLenum)=0;
  if(!real) real=(void(*)(GLenum,GLenum,GLenum))dlsym(RTLD_DEFAULT,"glStencilOp");
  GL_TRACE("glStencilOp", "fail=0x%x zfail=0x%x zpass=0x%x", fail, zfail, zpass);
  if(real) real(fail,zfail,zpass);
}
SF void sf_glStencilMask(GLuint mask){
  static void(*real)(GLuint)=0;
  if(!real) real=(void(*)(GLuint))dlsym(RTLD_DEFAULT,"glStencilMask");
  GL_TRACE("glStencilMask", "0x%x", mask);
  if(real) real(mask);
}
SF void sf_glClearStencil(GLint s){
  static void(*real)(GLint)=0;
  if(!real) real=(void(*)(GLint))dlsym(RTLD_DEFAULT,"glClearStencil");
  GL_TRACE("glClearStencil", "%d", s);
  if(real) real(s);
}
SF void sf_glCullFace(GLenum mode){
  static void(*real)(GLenum)=0;
  if(!real) real=(void(*)(GLenum))dlsym(RTLD_DEFAULT,"glCullFace");
  GL_TRACE("glCullFace", "0x%x(%s)", mode, gl_enum_name(mode));
  if(real) real(mode);
}
SF void sf_glFrontFace(GLenum mode){
  static void(*real)(GLenum)=0;
  if(!real) real=(void(*)(GLenum))dlsym(RTLD_DEFAULT,"glFrontFace");
  GL_TRACE("glFrontFace", "0x%x(%s)", mode, gl_enum_name(mode));
  if(real) real(mode);
}
SF void sf_glVertexAttribPointer(GLuint index,GLint size,GLenum type,GLboolean normalized,
                                 GLsizei stride,const void *pointer){
  static void(*real)(GLuint,GLint,GLenum,GLboolean,GLsizei,const void*)=0;
  if(!real) real=(void(*)(GLuint,GLint,GLenum,GLboolean,GLsizei,const void*))dlsym(RTLD_DEFAULT,"glVertexAttribPointer");
  GL_TRACE("glVertexAttribPointer", "idx=%u size=%d type=0x%x(%s) norm=%u stride=%d ptr=%p",
           index, size, type, gl_enum_name(type), normalized, stride, pointer);
  if(real) real(index,size,type,normalized,stride,pointer);
}
SF void sf_glEnableVertexAttribArray(GLuint index){
  static void(*real)(GLuint)=0;
  if(!real) real=(void(*)(GLuint))dlsym(RTLD_DEFAULT,"glEnableVertexAttribArray");
  GL_TRACE("glEnableVertexAttribArray", "%u", index);
  if(real) real(index);
}
SF void sf_glDisableVertexAttribArray(GLuint index){
  static void(*real)(GLuint)=0;
  if(!real) real=(void(*)(GLuint))dlsym(RTLD_DEFAULT,"glDisableVertexAttribArray");
  GL_TRACE("glDisableVertexAttribArray", "%u", index);
  if(real) real(index);
}

SF GLint sf_glGetUniformLocation(GLuint program,const GLchar *name){
  static GLint(*real)(GLuint,const GLchar*)=0;
  if(!real) real=(GLint(*)(GLuint,const GLchar*))dlsym(RTLD_DEFAULT,"glGetUniformLocation");
  GLint r = real ? real(program,name) : -1;
  GL_TRACE("glGetUniformLocation", "program=%u name='%s' -> %d", program, name ? name : "(null)", r);
  return r;
}
SF GLint sf_glGetAttribLocation(GLuint program,const GLchar *name){
  static GLint(*real)(GLuint,const GLchar*)=0;
  if(!real) real=(GLint(*)(GLuint,const GLchar*))dlsym(RTLD_DEFAULT,"glGetAttribLocation");
  GLint r = real ? real(program,name) : -1;
  GL_TRACE("glGetAttribLocation", "program=%u name='%s' -> %d", program, name ? name : "(null)", r);
  return r;
}
SF void sf_glUniform1fv(GLint loc,GLsizei count,const GLfloat *value){
  static void(*real)(GLint,GLsizei,const GLfloat*)=0;
  if(!real) real=(void(*)(GLint,GLsizei,const GLfloat*))dlsym(RTLD_DEFAULT,"glUniform1fv");
  GL_TRACE("glUniform1fv", "loc=%d count=%d v=%p %.4f",
           loc, count, value, value ? value[0] : 0.0f);
  if(real) real(loc,count,value);
}
SF void sf_glUniform1i(GLint loc,GLint v0){
  static void(*real)(GLint,GLint)=0;
  if(!real) real=(void(*)(GLint,GLint))dlsym(RTLD_DEFAULT,"glUniform1i");
  GL_TRACE("glUniform1i", "loc=%d v=%d", loc, v0);
  if(real) real(loc,v0);
}
SF void sf_glUniform1iv(GLint loc,GLsizei count,const GLint *value){
  static void(*real)(GLint,GLsizei,const GLint*)=0;
  if(!real) real=(void(*)(GLint,GLsizei,const GLint*))dlsym(RTLD_DEFAULT,"glUniform1iv");
  GL_TRACE("glUniform1iv", "loc=%d count=%d v=%p %d",
           loc, count, value, value ? value[0] : 0);
  if(real) real(loc,count,value);
}
SF void sf_glUniform2fv(GLint loc,GLsizei count,const GLfloat *value){
  static void(*real)(GLint,GLsizei,const GLfloat*)=0;
  if(!real) real=(void(*)(GLint,GLsizei,const GLfloat*))dlsym(RTLD_DEFAULT,"glUniform2fv");
  GL_TRACE("glUniform2fv", "loc=%d count=%d v=%p %.4f %.4f",
           loc, count, value, value ? value[0] : 0.0f, value ? value[1] : 0.0f);
  if(real) real(loc,count,value);
}
SF void sf_glUniform2iv(GLint loc,GLsizei count,const GLint *value){
  static void(*real)(GLint,GLsizei,const GLint*)=0;
  if(!real) real=(void(*)(GLint,GLsizei,const GLint*))dlsym(RTLD_DEFAULT,"glUniform2iv");
  GL_TRACE("glUniform2iv", "loc=%d count=%d v=%p %d %d",
           loc, count, value, value ? value[0] : 0, value ? value[1] : 0);
  if(real) real(loc,count,value);
}
SF void sf_glUniform3fv(GLint loc,GLsizei count,const GLfloat *value){
  static void(*real)(GLint,GLsizei,const GLfloat*)=0;
  if(!real) real=(void(*)(GLint,GLsizei,const GLfloat*))dlsym(RTLD_DEFAULT,"glUniform3fv");
  GL_TRACE("glUniform3fv", "loc=%d count=%d v=%p %.4f %.4f %.4f",
           loc, count, value, value ? value[0] : 0.0f,
           value ? value[1] : 0.0f, value ? value[2] : 0.0f);
  if(real) real(loc,count,value);
}
SF void sf_glUniform3iv(GLint loc,GLsizei count,const GLint *value){
  static void(*real)(GLint,GLsizei,const GLint*)=0;
  if(!real) real=(void(*)(GLint,GLsizei,const GLint*))dlsym(RTLD_DEFAULT,"glUniform3iv");
  GL_TRACE("glUniform3iv", "loc=%d count=%d v=%p %d %d %d",
           loc, count, value, value ? value[0] : 0,
           value ? value[1] : 0, value ? value[2] : 0);
  if(real) real(loc,count,value);
}
SF void sf_glUniform4fv(GLint loc,GLsizei count,const GLfloat *value){
  static void(*real)(GLint,GLsizei,const GLfloat*)=0;
  if(!real) real=(void(*)(GLint,GLsizei,const GLfloat*))dlsym(RTLD_DEFAULT,"glUniform4fv");
  GL_TRACE("glUniform4fv", "loc=%d count=%d v=%p %.4f %.4f %.4f %.4f",
           loc, count, value, value ? value[0] : 0.0f,
           value ? value[1] : 0.0f, value ? value[2] : 0.0f,
           value ? value[3] : 0.0f);
  if(real) real(loc,count,value);
}
SF void sf_glUniform4iv(GLint loc,GLsizei count,const GLint *value){
  static void(*real)(GLint,GLsizei,const GLint*)=0;
  if(!real) real=(void(*)(GLint,GLsizei,const GLint*))dlsym(RTLD_DEFAULT,"glUniform4iv");
  GL_TRACE("glUniform4iv", "loc=%d count=%d v=%p %d %d %d %d",
           loc, count, value, value ? value[0] : 0,
           value ? value[1] : 0, value ? value[2] : 0, value ? value[3] : 0);
  if(real) real(loc,count,value);
}
SF void sf_glUniformMatrix2fv(GLint loc,GLsizei count,GLboolean transpose,const GLfloat *value){
  static void(*real)(GLint,GLsizei,GLboolean,const GLfloat*)=0;
  if(!real) real=(void(*)(GLint,GLsizei,GLboolean,const GLfloat*))dlsym(RTLD_DEFAULT,"glUniformMatrix2fv");
  GL_TRACE("glUniformMatrix2fv", "loc=%d count=%d tr=%u v=%p %.4f %.4f %.4f %.4f",
           loc, count, transpose, value, value ? value[0] : 0.0f,
           value ? value[1] : 0.0f, value ? value[2] : 0.0f, value ? value[3] : 0.0f);
  if(real) real(loc,count,transpose,value);
}
SF void sf_glUniformMatrix3fv(GLint loc,GLsizei count,GLboolean transpose,const GLfloat *value){
  static void(*real)(GLint,GLsizei,GLboolean,const GLfloat*)=0;
  if(!real) real=(void(*)(GLint,GLsizei,GLboolean,const GLfloat*))dlsym(RTLD_DEFAULT,"glUniformMatrix3fv");
  GL_TRACE("glUniformMatrix3fv", "loc=%d count=%d tr=%u v=%p %.4f %.4f %.4f %.4f",
           loc, count, transpose, value, value ? value[0] : 0.0f,
           value ? value[1] : 0.0f, value ? value[2] : 0.0f, value ? value[3] : 0.0f);
  if(real) real(loc,count,transpose,value);
}
SF void sf_glUniformMatrix4fv(GLint loc,GLsizei count,GLboolean transpose,const GLfloat *value){
  static void(*real)(GLint,GLsizei,GLboolean,const GLfloat*)=0;
  if(!real) real=(void(*)(GLint,GLsizei,GLboolean,const GLfloat*))dlsym(RTLD_DEFAULT,"glUniformMatrix4fv");
  GL_TRACE("glUniformMatrix4fv", "loc=%d count=%d tr=%u v=%p %.4f %.4f %.4f %.4f",
           loc, count, transpose, value, value ? value[0] : 0.0f,
           value ? value[1] : 0.0f, value ? value[2] : 0.0f, value ? value[3] : 0.0f);
  if(real) real(loc,count,transpose,value);
}
SF void sf_glBindRenderbuffer(GLenum target,GLuint rb){
  static void(*real)(GLenum,GLuint)=0;
  if(!real) real=(void(*)(GLenum,GLuint))dlsym(RTLD_DEFAULT,"glBindRenderbuffer");
  GL_TRACE("glBindRenderbuffer", "target=0x%x(%s) rb=%u", target, gl_enum_name(target), rb);
  if(real) real(target,rb);
}
SF void sf_glRenderbufferStorage(GLenum target,GLenum internalformat,GLsizei width,GLsizei height){
  static void(*real)(GLenum,GLenum,GLsizei,GLsizei)=0;
  if(!real) real=(void(*)(GLenum,GLenum,GLsizei,GLsizei))dlsym(RTLD_DEFAULT,"glRenderbufferStorage");
  GL_TRACE("glRenderbufferStorage", "target=0x%x(%s) ifmt=0x%x(%s) %dx%d",
           target, gl_enum_name(target), internalformat, gl_enum_name(internalformat), width, height);
  if(real) real(target,internalformat,width,height);
}
SF void sf_glFramebufferTexture2D(GLenum target,GLenum attachment,GLenum textarget,
                                  GLuint texture,GLint level){
  static void(*real)(GLenum,GLenum,GLenum,GLuint,GLint)=0;
  if(!real) real=(void(*)(GLenum,GLenum,GLenum,GLuint,GLint))dlsym(RTLD_DEFAULT,"glFramebufferTexture2D");
  GL_TRACE("glFramebufferTexture2D", "target=0x%x(%s) attach=0x%x(%s) textarget=0x%x(%s) tex=%u level=%d",
           target, gl_enum_name(target), attachment, gl_enum_name(attachment),
           textarget, gl_enum_name(textarget), texture, level);
  if(real) real(target,attachment,textarget,texture,level);
}
SF void sf_glFramebufferRenderbuffer(GLenum target,GLenum attachment,GLenum renderbuffertarget,
                                     GLuint renderbuffer){
  static void(*real)(GLenum,GLenum,GLenum,GLuint)=0;
  if(!real) real=(void(*)(GLenum,GLenum,GLenum,GLuint))dlsym(RTLD_DEFAULT,"glFramebufferRenderbuffer");
  GL_TRACE("glFramebufferRenderbuffer", "target=0x%x(%s) attach=0x%x(%s) rbtarget=0x%x(%s) rb=%u",
           target, gl_enum_name(target), attachment, gl_enum_name(attachment),
           renderbuffertarget, gl_enum_name(renderbuffertarget), renderbuffer);
  if(real) real(target,attachment,renderbuffertarget,renderbuffer);
}
SF GLenum sf_glCheckFramebufferStatus(GLenum target){
  static GLenum(*real)(GLenum)=0;
  if(!real) real=(GLenum(*)(GLenum))dlsym(RTLD_DEFAULT,"glCheckFramebufferStatus");
  GLenum r = real ? real(target) : 0;
  GL_TRACE("glCheckFramebufferStatus", "target=0x%x(%s) -> 0x%x(%s)",
           target, gl_enum_name(target), r, gl_enum_name(r));
  return r;
}

static const char *gl_enum_name(GLenum e) {
  switch (e) {
  case 0x0000: return "ZERO/POINTS";
  case 0x0001: return "ONE/LINES";
  case 0x0003: return "LINE_STRIP";
  case 0x0004: return "TRIANGLES";
  case 0x0005: return "TRIANGLE_STRIP";
  case 0x0006: return "TRIANGLE_FAN";
  case 0x0200: return "NEVER";
  case 0x0201: return "LESS";
  case 0x0202: return "EQUAL";
  case 0x0203: return "LEQUAL";
  case 0x0204: return "GREATER";
  case 0x0205: return "NOTEQUAL";
  case 0x0206: return "GEQUAL";
  case 0x0207: return "ALWAYS";
  case 0x0300: return "SRC_COLOR";
  case 0x0301: return "ONE_MINUS_SRC_COLOR";
  case 0x0302: return "SRC_ALPHA";
  case 0x0303: return "ONE_MINUS_SRC_ALPHA";
  case 0x0304: return "DST_ALPHA";
  case 0x0305: return "ONE_MINUS_DST_ALPHA";
  case 0x0306: return "DST_COLOR";
  case 0x0307: return "ONE_MINUS_DST_COLOR";
  case 0x0308: return "SRC_ALPHA_SATURATE";
  case 0x0404: return "FRONT";
  case 0x0405: return "BACK";
  case 0x0408: return "FRONT_AND_BACK";
  case 0x0500: return "INVALID_ENUM";
  case 0x0501: return "INVALID_VALUE";
  case 0x0502: return "INVALID_OPERATION";
  case 0x0600: return "EXP";
  case 0x0601: return "EXP2";
  case 0x0901: return "CCW";
  case 0x0900: return "CW";
  case 0x0B44: return "CULL_FACE";
  case 0x0B71: return "DEPTH_TEST";
  case 0x0B90: return "STENCIL_TEST";
  case 0x0BE2: return "BLEND";
  case 0x0C11: return "SCISSOR_TEST";
  case 0x0DE1: return "TEXTURE_2D";
  case 0x1400: return "BYTE";
  case 0x1402: return "SHORT";
  case 0x1403: return "UNSIGNED_SHORT";
  case 0x1404: return "INT";
  case 0x1405: return "UNSIGNED_INT";
  case 0x1406: return "FLOAT";
  case 0x8D64: return "ETC1_RGB8_OES";
  case 0x8C00: return "PVRTC_RGB_4BPPV1_IMG";
  case 0x8C01: return "PVRTC_RGB_2BPPV1_IMG";
  case 0x8C02: return "PVRTC_RGBA_4BPPV1_IMG";
  case 0x8C03: return "PVRTC_RGBA_2BPPV1_IMG";
  case 0x83F0: return "DXT1_RGB";
  case 0x83F1: return "DXT1_RGBA";
  case 0x83F2: return "DXT3_RGBA";
  case 0x83F3: return "DXT5_RGBA";
  case 0x1907: return "RGB";
  case 0x1908: return "RGBA";
  case 0x1902: return "DEPTH_COMPONENT";
  case 0x1401: return "UNSIGNED_BYTE";
  case 0x8363: return "UNSIGNED_SHORT_5_6_5";
  case 0x8033: return "UNSIGNED_SHORT_4_4_4_4";
  case 0x8034: return "UNSIGNED_SHORT_5_5_5_1";
  case 0x8006: return "FUNC_ADD";
  case 0x800A: return "FUNC_SUBTRACT";
  case 0x800B: return "FUNC_REVERSE_SUBTRACT";
  case 0x812F: return "CLAMP_TO_EDGE";
  case 0x2600: return "NEAREST";
  case 0x2601: return "LINEAR";
  case 0x2700: return "NEAREST_MIPMAP_NEAREST";
  case 0x2701: return "LINEAR_MIPMAP_NEAREST";
  case 0x2702: return "NEAREST_MIPMAP_LINEAR";
  case 0x2703: return "LINEAR_MIPMAP_LINEAR";
  case 0x2800: return "TEXTURE_MAG_FILTER";
  case 0x2801: return "TEXTURE_MIN_FILTER";
  case 0x2802: return "TEXTURE_WRAP_S";
  case 0x2803: return "TEXTURE_WRAP_T";
  case 0x84C0: return "TEXTURE0";
  case 0x84C1: return "TEXTURE1";
  case 0x8892: return "ARRAY_BUFFER";
  case 0x8893: return "ELEMENT_ARRAY_BUFFER";
  case 0x8B30: return "FRAGMENT_SHADER";
  case 0x8B31: return "VERTEX_SHADER";
  case 0x8B80: return "DELETE_STATUS";
  case 0x8B81: return "COMPILE_STATUS";
  case 0x8B82: return "LINK_STATUS";
  case 0x8B84: return "INFO_LOG_LENGTH";
  case 0x8CA6: return "FRAMEBUFFER_BINDING";
  case 0x8CD5: return "FRAMEBUFFER_COMPLETE";
  case 0x8CD6: return "FRAMEBUFFER_INCOMPLETE_ATTACHMENT";
  case 0x8CD7: return "FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT";
  case 0x8CD9: return "FRAMEBUFFER_INCOMPLETE_DIMENSIONS";
  case 0x8CDD: return "FRAMEBUFFER_UNSUPPORTED";
  case 0x8CE0: return "COLOR_ATTACHMENT0";
  case 0x8D00: return "DEPTH_ATTACHMENT";
  case 0x8D20: return "STENCIL_ATTACHMENT";
  case 0x8D40: return "FRAMEBUFFER";
  case 0x8D41: return "RENDERBUFFER";
  default: return "?";
  }
}

static GLenum checked_gl_error(void) {
  static GLenum (*real)(void) = 0;
  if (!real)
    real = (GLenum(*)(void))dlsym(RTLD_DEFAULT, "glGetError");
  return real ? real() : 0;
}

static void log_current_pixel(const char *tag, int n) {
  if (!gl_trace_enabled() || !GL_SHOULD_LOG(n))
    return;

  static void (*getiv)(GLenum, GLint *) = 0;
  static void (*readpix)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *) = 0;
  if (!getiv)
    getiv = (void(*)(GLenum, GLint *))dlsym(RTLD_DEFAULT, "glGetIntegerv");
  if (!readpix)
    readpix = (void(*)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *))
        dlsym(RTLD_DEFAULT, "glReadPixels");
  if (!getiv || !readpix)
    return;

  GLint vp[4] = {0, 0, 0, 0};
  GLint fb = -1;
  unsigned char px[4] = {0, 0, 0, 0};
  getiv(0x0BA2, vp);   /* GL_VIEWPORT */
  getiv(0x8CA6, &fb);  /* GL_FRAMEBUFFER_BINDING */
  if (vp[2] <= 0 || vp[3] <= 0)
    return;

  GLint x = vp[0] + vp[2] / 2;
  GLint y = vp[1] + vp[3] / 2;
  readpix(x, y, 1, 1, 0x1908, 0x1401, px); /* RGBA/UNSIGNED_BYTE */
  GLenum err = checked_gl_error();
  fprintf(stderr,
          "[GL] pixel after %s#%d fb=%d vp=%d,%d %dx%d center=%d,%d rgba=%u,%u,%u,%u err=0x%x\n",
          tag, n, fb, vp[0], vp[1], vp[2], vp[3], x, y,
          px[0], px[1], px[2], px[3], err);
}

static void log_draw_state(const char *tag, int n) {
  if (!gl_trace_enabled() || !GL_SHOULD_LOG(n))
    return;

  fprintf(stderr,
          "[GLSTATE] %s#%d prog=%u active=0x%x(%s) tex2d[0..3]=%u,%u,%u,%u\n",
          tag, n, g_current_program, g_active_texture,
          gl_enum_name(g_active_texture), g_bound_tex2d[0], g_bound_tex2d[1],
          g_bound_tex2d[2], g_bound_tex2d[3]);
}

SF void sf_glCompressedTexImage2D(GLenum target,GLint level,GLenum internalformat,
                                  GLsizei width,GLsizei height,GLint border,
                                  GLsizei imageSize,const void *data){
  static void(*real)(GLenum,GLint,GLenum,GLsizei,GLsizei,GLint,GLsizei,const void*)=0;
  if(!real) real=(void(*)(GLenum,GLint,GLenum,GLsizei,GLsizei,GLint,GLsizei,const void*))dlsym(RTLD_DEFAULT,"glCompressedTexImage2D");
  GL_TRACE("glCompressedTexImage2D", "fmt=0x%x(%s) level=%d %dx%d size=%d",
           internalformat, gl_enum_name(internalformat), level, width, height, imageSize);
  if(real) real(target,level,internalformat,width,height,border,imageSize,data);
  if (gl_trace_enabled()) {
    GLenum err = checked_gl_error();
    if (err)
      fprintf(stderr, "[GL] glCompressedTexImage2D error=0x%x fmt=0x%x(%s) %dx%d size=%d\n",
              err, internalformat, gl_enum_name(internalformat), width, height, imageSize);
  }
}

SF void sf_glCompressedTexSubImage2D(GLenum target,GLint level,GLint xoffset,
                                     GLint yoffset,GLsizei width,GLsizei height,
                                     GLenum format,GLsizei imageSize,const void *data){
  static void(*real)(GLenum,GLint,GLint,GLint,GLsizei,GLsizei,GLenum,GLsizei,const void*)=0;
  if(!real) real=(void(*)(GLenum,GLint,GLint,GLint,GLsizei,GLsizei,GLenum,GLsizei,const void*))dlsym(RTLD_DEFAULT,"glCompressedTexSubImage2D");
  GL_TRACE("glCompressedTexSubImage2D", "fmt=0x%x(%s) level=%d xy=%d,%d %dx%d size=%d",
           format, gl_enum_name(format), level, xoffset, yoffset, width, height, imageSize);
  if(real) real(target,level,xoffset,yoffset,width,height,format,imageSize,data);
  if (gl_trace_enabled()) {
    GLenum err = checked_gl_error();
    if (err)
      fprintf(stderr, "[GL] glCompressedTexSubImage2D error=0x%x fmt=0x%x(%s) %dx%d size=%d\n",
              err, format, gl_enum_name(format), width, height, imageSize);
  }
}

SF void sf_glTexImage2D(GLenum target,GLint level,GLint internalformat,
                        GLsizei width,GLsizei height,GLint border,
                        GLenum format,GLenum type,const void *pixels){
  static void(*real)(GLenum,GLint,GLint,GLsizei,GLsizei,GLint,GLenum,GLenum,const void*)=0;
  if(!real) real=(void(*)(GLenum,GLint,GLint,GLsizei,GLsizei,GLint,GLenum,GLenum,const void*))dlsym(RTLD_DEFAULT,"glTexImage2D");
  GL_TRACE("glTexImage2D", "ifmt=0x%x(%s) fmt=0x%x(%s) type=0x%x(%s) level=%d %dx%d",
           internalformat, gl_enum_name((GLenum)internalformat), format,
           gl_enum_name(format), type, gl_enum_name(type), level, width, height);
  if(real) real(target,level,internalformat,width,height,border,format,type,pixels);
  if (gl_trace_enabled()) {
    GLenum err = checked_gl_error();
    if (err)
      fprintf(stderr, "[GL] glTexImage2D error=0x%x ifmt=0x%x fmt=0x%x type=0x%x %dx%d\n",
              err, internalformat, format, type, width, height);
  }
}

SF void sf_glTexSubImage2D(GLenum target,GLint level,GLint xoffset,GLint yoffset,
                           GLsizei width,GLsizei height,GLenum format,
                           GLenum type,const void *pixels){
  static void(*real)(GLenum,GLint,GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,const void*)=0;
  if(!real) real=(void(*)(GLenum,GLint,GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,const void*))dlsym(RTLD_DEFAULT,"glTexSubImage2D");
  GL_TRACE("glTexSubImage2D", "fmt=0x%x(%s) type=0x%x(%s) level=%d xy=%d,%d %dx%d",
           format, gl_enum_name(format), type, gl_enum_name(type), level,
           xoffset, yoffset, width, height);
  if(real) real(target,level,xoffset,yoffset,width,height,format,type,pixels);
  if (gl_trace_enabled()) {
    GLenum err = checked_gl_error();
    if (err)
      fprintf(stderr, "[GL] glTexSubImage2D error=0x%x fmt=0x%x type=0x%x %dx%d\n",
              err, format, type, width, height);
  }
}

SF void sf_glShaderSource(GLuint shader,GLsizei count,const GLchar **string,
                          const GLint *length){
  static void(*real)(GLuint,GLsizei,const GLchar **,const GLint*)=0;
  if(!real) real=(void(*)(GLuint,GLsizei,const GLchar **,const GLint*))dlsym(RTLD_DEFAULT,"glShaderSource");
  if (gl_trace_enabled() && string && string[0]) {
    int n = length && length[0] >= 0 ? length[0] : (int)strlen(string[0]);
    if (n > 96) n = 96;
    fprintf(stderr, "[GL] glShaderSource shader=%u count=%d first='%.*s'\n",
            shader, count, n, string[0]);
  }
  if(real) real(shader,count,string,length);
}

SF void sf_glCompileShader(GLuint shader){
  static void(*real)(GLuint)=0;
  if(!real) real=(void(*)(GLuint))dlsym(RTLD_DEFAULT,"glCompileShader");
  GL_TRACE("glCompileShader", "%u", shader);
  if(real) real(shader);
}

SF void sf_glGetShaderiv(GLuint shader,GLenum pname,GLint *params){
  static void(*real)(GLuint,GLenum,GLint*)=0;
  if(!real) real=(void(*)(GLuint,GLenum,GLint*))dlsym(RTLD_DEFAULT,"glGetShaderiv");
  if(real) real(shader,pname,params);
  if (gl_trace_enabled() && (pname == 0x8B81 || pname == 0x8B84))
    fprintf(stderr, "[GL] glGetShaderiv shader=%u pname=0x%x -> %d\n",
            shader, pname, params ? *params : -1);
}

SF void sf_glGetShaderInfoLog(GLuint shader,GLsizei maxLength,GLsizei *length,
                              GLchar *infoLog){
  static void(*real)(GLuint,GLsizei,GLsizei*,GLchar*)=0;
  if(!real) real=(void(*)(GLuint,GLsizei,GLsizei*,GLchar*))dlsym(RTLD_DEFAULT,"glGetShaderInfoLog");
  if(real) real(shader,maxLength,length,infoLog);
  if (gl_trace_enabled() && infoLog && infoLog[0])
    fprintf(stderr, "[GL] glGetShaderInfoLog shader=%u '%s'\n", shader, infoLog);
}

SF void sf_glLinkProgram(GLuint program){
  static void(*real)(GLuint)=0;
  if(!real) real=(void(*)(GLuint))dlsym(RTLD_DEFAULT,"glLinkProgram");
  GL_TRACE("glLinkProgram", "%u", program);
  if(real) real(program);
}

SF void sf_glGetProgramiv(GLuint program,GLenum pname,GLint *params){
  static void(*real)(GLuint,GLenum,GLint*)=0;
  if(!real) real=(void(*)(GLuint,GLenum,GLint*))dlsym(RTLD_DEFAULT,"glGetProgramiv");
  if(real) real(program,pname,params);
  if (gl_trace_enabled() && (pname == 0x8B82 || pname == 0x8B84))
    fprintf(stderr, "[GL] glGetProgramiv program=%u pname=0x%x -> %d\n",
            program, pname, params ? *params : -1);
}

SF void sf_glGetProgramInfoLog(GLuint program,GLsizei maxLength,GLsizei *length,
                               GLchar *infoLog){
  static void(*real)(GLuint,GLsizei,GLsizei*,GLchar*)=0;
  if(!real) real=(void(*)(GLuint,GLsizei,GLsizei*,GLchar*))dlsym(RTLD_DEFAULT,"glGetProgramInfoLog");
  if(real) real(program,maxLength,length,infoLog);
  if (gl_trace_enabled() && infoLog && infoLog[0])
    fprintf(stderr, "[GL] glGetProgramInfoLog program=%u '%s'\n", program, infoLog);
}

SF GLenum sf_glGetError(void){
  GLenum err = checked_gl_error();
  if (gl_trace_enabled() && err)
    fprintf(stderr, "[GL] glGetError -> 0x%x\n", err);
  return err;
}

struct sfent { const char *nm; void *fn; };
static const struct sfent SFTAB[] = {
  {"acos",sf_acos},{"asin",sf_asin},{"atan",sf_atan},{"cos",sf_cos},{"sin",sf_sin},{"tan",sf_tan},
  {"cosh",sf_cosh},{"sinh",sf_sinh},{"tanh",sf_tanh},
  {"exp",sf_exp},{"exp2",sf_exp2},{"log",sf_log},{"log10",sf_log10},{"sqrt",sf_sqrt},
  {"ceil",sf_ceil},{"floor",sf_floor},{"round",sf_round},{"trunc",sf_trunc},{"rint",sf_rint},
  {"acosf",sf_acosf},{"asinf",sf_asinf},{"atanf",sf_atanf},{"cosf",sf_cosf},{"sinf",sf_sinf},{"tanf",sf_tanf},
  {"expf",sf_expf},{"logf",sf_logf},{"sqrtf",sf_sqrtf},{"fabsf",sf_fabsf},
  {"ceilf",sf_ceilf},{"floorf",sf_floorf},{"roundf",sf_roundf},{"truncf",sf_truncf},
  {"atan2",sf_atan2},{"fmod",sf_fmod},{"pow",sf_pow},{"remainder",sf_remainder},
  {"atan2f",sf_atan2f},{"fmodf",sf_fmodf},{"powf",sf_powf},
  {"modf",sf_modf},{"modff",sf_modff},{"frexp",sf_frexp},{"ldexp",sf_ldexp},{"strtod",sf_strtod},
  {"glClearColor",sf_glClearColor},
  {"glBlendColor",sf_glBlendColor},{"glClearDepthf",sf_glClearDepthf},
  {"glDepthRangef",sf_glDepthRangef},{"glLineWidth",sf_glLineWidth},
  {"glPolygonOffset",sf_glPolygonOffset},{"glSampleCoverage",sf_glSampleCoverage},
  {"glTexParameterf",sf_glTexParameterf},{"glUniform1f",sf_glUniform1f},
  {"glVertexAttrib4f",sf_glVertexAttrib4f},
  {"glViewport",sf_glViewport},{"glClear",sf_glClear},
  {"glDrawArrays",sf_glDrawArrays},{"glDrawElements",sf_glDrawElements},
  {"glBindFramebuffer",sf_glBindFramebuffer},{"glUseProgram",sf_glUseProgram},
  {"glActiveTexture",sf_glActiveTexture},{"glBindTexture",sf_glBindTexture},
  {"glTexParameteri",sf_glTexParameteri},
  {"glBindBuffer",sf_glBindBuffer},{"glBufferData",sf_glBufferData},
  {"glBufferSubData",sf_glBufferSubData},
  {"glEnable",sf_glEnable},{"glDisable",sf_glDisable},
  {"glBlendFunc",sf_glBlendFunc},{"glBlendFuncSeparate",sf_glBlendFuncSeparate},
  {"glBlendEquation",sf_glBlendEquation},
  {"glBlendEquationSeparate",sf_glBlendEquationSeparate},
  {"glColorMask",sf_glColorMask},{"glDepthMask",sf_glDepthMask},
  {"glDepthFunc",sf_glDepthFunc},{"glScissor",sf_glScissor},
  {"glStencilFunc",sf_glStencilFunc},{"glStencilOp",sf_glStencilOp},
  {"glStencilMask",sf_glStencilMask},{"glClearStencil",sf_glClearStencil},
  {"glCullFace",sf_glCullFace},{"glFrontFace",sf_glFrontFace},
  {"glVertexAttribPointer",sf_glVertexAttribPointer},
  {"glEnableVertexAttribArray",sf_glEnableVertexAttribArray},
  {"glDisableVertexAttribArray",sf_glDisableVertexAttribArray},
  {"glGetUniformLocation",sf_glGetUniformLocation},
  {"glGetAttribLocation",sf_glGetAttribLocation},
  {"glUniform1fv",sf_glUniform1fv},{"glUniform1i",sf_glUniform1i},
  {"glUniform1iv",sf_glUniform1iv},{"glUniform2fv",sf_glUniform2fv},
  {"glUniform2iv",sf_glUniform2iv},{"glUniform3fv",sf_glUniform3fv},
  {"glUniform3iv",sf_glUniform3iv},{"glUniform4fv",sf_glUniform4fv},
  {"glUniform4iv",sf_glUniform4iv},
  {"glUniformMatrix2fv",sf_glUniformMatrix2fv},
  {"glUniformMatrix3fv",sf_glUniformMatrix3fv},
  {"glUniformMatrix4fv",sf_glUniformMatrix4fv},
  {"glBindRenderbuffer",sf_glBindRenderbuffer},
  {"glRenderbufferStorage",sf_glRenderbufferStorage},
  {"glFramebufferTexture2D",sf_glFramebufferTexture2D},
  {"glFramebufferRenderbuffer",sf_glFramebufferRenderbuffer},
  {"glCheckFramebufferStatus",sf_glCheckFramebufferStatus},
  {"glCompressedTexImage2D",sf_glCompressedTexImage2D},
  {"glCompressedTexSubImage2D",sf_glCompressedTexSubImage2D},
  {"glTexImage2D",sf_glTexImage2D},{"glTexSubImage2D",sf_glTexSubImage2D},
  {"glShaderSource",sf_glShaderSource},{"glCompileShader",sf_glCompileShader},
  {"glGetShaderiv",sf_glGetShaderiv},{"glGetShaderInfoLog",sf_glGetShaderInfoLog},
  {"glLinkProgram",sf_glLinkProgram},{"glGetProgramiv",sf_glGetProgramiv},
  {"glGetProgramInfoLog",sf_glGetProgramInfoLog},{"glGetError",sf_glGetError},
};

void *softfp_resolve(const char *nm){
  if(!nm) return 0;
  for(unsigned i=0;i<sizeof(SFTAB)/sizeof(SFTAB[0]);i++)
    if(!strcmp(nm,SFTAB[i].nm)) return SFTAB[i].fn;
  return 0;
}
