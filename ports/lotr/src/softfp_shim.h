/* softfp_shim.h -- softfp(aapcs)->hardfp bridge wrappers for libm + GLES1. */
#ifndef SOFTFP_SHIM_H
#define SOFTFP_SHIM_H
#include <GLES/gl.h>
#define SF __attribute__((pcs("aapcs")))
/* math (double) */
SF double sf_acos(double), sf_asin(double), sf_atan(double), sf_cos(double), sf_sin(double), sf_tan(double);
SF double sf_exp(double), sf_log(double), sf_log10(double), sf_sqrt(double), sf_ceil(double), sf_floor(double);
SF double sf_atan2(double,double), sf_fmod(double,double), sf_pow(double,double);
/* math (float) */
SF float sf_acosf(float), sf_asinf(float), sf_atanf(float), sf_cosf(float), sf_sinf(float), sf_tanf(float);
SF float sf_expf(float), sf_logf(float), sf_log10f(float), sf_sqrtf(float), sf_fabsf(float);
SF float sf_ceilf(float), sf_floorf(float);
SF float sf_atan2f(float,float), sf_fmodf(float,float), sf_powf(float,float);
SF void sf_sincosf(float,float*,float*);
/* GLES1 float entry points */
SF void sf_glClearColor(GLclampf,GLclampf,GLclampf,GLclampf);
SF void sf_glClearDepthf(GLclampf);
SF void sf_glDepthRangef(GLclampf,GLclampf);
SF void sf_glAlphaFunc(GLenum,GLclampf);
SF void sf_glFogf(GLenum,GLfloat);
SF void sf_glLightf(GLenum,GLenum,GLfloat);
SF void sf_glMaterialf(GLenum,GLenum,GLfloat);
SF void sf_glTexEnvf(GLenum,GLenum,GLfloat);
SF void sf_glNormal3f(GLfloat,GLfloat,GLfloat);
#endif
