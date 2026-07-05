#define _GNU_SOURCE
#include <dlfcn.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SF __attribute__((pcs("aapcs")))

/* libDeadSpace.so is old armeabi/softfp. NextOS armhf libs are hardfp.
 * Route every imported function that takes float/double by value through an
 * AAPCS wrapper so GCC moves arguments between core and VFP registers. */
#define W1D(n) SF double sf_##n(double x) { return n(x); }
#define W1F(n) SF float sf_##n(float x) { return n(x); }
#define W2D(n) SF double sf_##n(double a, double b) { return n(a, b); }
#define W2F(n) SF float sf_##n(float a, float b) { return n(a, b); }

W1D(acos) W1D(atan) W1D(ceil) W1D(cos) W1D(exp) W1D(floor) W1D(log)
W1D(log10) W1D(sin) W1D(sqrt) W1D(tan)
W1F(acosf) W1F(asinf) W1F(ceilf) W1F(cosf) W1F(expf)
W1F(floorf) W1F(log10f) W1F(logf) W1F(sinf) W1F(sqrtf) W1F(tanf)
W2D(atan2) W2D(pow)
W2F(atan2f) W2F(fmodf) W2F(powf)

SF double sf_ldexp(double x, int e) { return ldexp(x, e); }
SF double sf_modf(double x, double *iptr) { return modf(x, iptr); }
SF double sf_strtod(const char *s, char **end) { return strtod(s, end); }
SF int sf___isinf(double x) { return isinf(x); }

typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef int GLint;
typedef int GLsizei;
typedef float GLfloat;

extern void deadspace_gl_prepare_draw(const char *kind, unsigned int mode, int count);

typedef void (*PFN_glAlphaFunc)(GLenum, GLfloat);
typedef void (*PFN_glClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (*PFN_glClearDepthf)(GLfloat);
typedef void (*PFN_glColor4f)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (*PFN_glDepthRangef)(GLfloat, GLfloat);
typedef void (*PFN_glDrawTexfOES)(GLfloat, GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (*PFN_glFogf)(GLenum, GLfloat);
typedef void (*PFN_glFrustumf)(GLfloat, GLfloat, GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (*PFN_glLightf)(GLenum, GLenum, GLfloat);
typedef void (*PFN_glLightModelf)(GLenum, GLfloat);
typedef void (*PFN_glLineWidth)(GLfloat);
typedef void (*PFN_glMaterialf)(GLenum, GLenum, GLfloat);
typedef void (*PFN_glMultiTexCoord4f)(GLenum, GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (*PFN_glNormal3f)(GLfloat, GLfloat, GLfloat);
typedef void (*PFN_glOrthof)(GLfloat, GLfloat, GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (*PFN_glPointParameterf)(GLenum, GLfloat);
typedef void (*PFN_glPointSize)(GLfloat);
typedef void (*PFN_glPolygonOffset)(GLfloat, GLfloat);
typedef void (*PFN_glRotatef)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (*PFN_glSampleCoverage)(GLfloat, GLboolean);
typedef void (*PFN_glScalef)(GLfloat, GLfloat, GLfloat);
typedef void (*PFN_glTexEnvf)(GLenum, GLenum, GLfloat);
typedef void (*PFN_glTexGenfOES)(GLenum, GLenum, GLfloat);
typedef void (*PFN_glTexParameterf)(GLenum, GLenum, GLfloat);
typedef void (*PFN_glTranslatef)(GLfloat, GLfloat, GLfloat);

#define GLREAL(name, type)                                                     \
  static type real_##name(void) {                                              \
    static type fn;                                                            \
    if (!fn) fn = (type)dlsym(RTLD_DEFAULT, #name);                            \
    return fn;                                                                 \
  }

GLREAL(glAlphaFunc, PFN_glAlphaFunc)
GLREAL(glClearColor, PFN_glClearColor)
GLREAL(glClearDepthf, PFN_glClearDepthf)
GLREAL(glColor4f, PFN_glColor4f)
GLREAL(glDepthRangef, PFN_glDepthRangef)
GLREAL(glDrawTexfOES, PFN_glDrawTexfOES)
GLREAL(glFogf, PFN_glFogf)
GLREAL(glFrustumf, PFN_glFrustumf)
GLREAL(glLightf, PFN_glLightf)
GLREAL(glLightModelf, PFN_glLightModelf)
GLREAL(glLineWidth, PFN_glLineWidth)
GLREAL(glMaterialf, PFN_glMaterialf)
GLREAL(glMultiTexCoord4f, PFN_glMultiTexCoord4f)
GLREAL(glNormal3f, PFN_glNormal3f)
GLREAL(glOrthof, PFN_glOrthof)
GLREAL(glPointParameterf, PFN_glPointParameterf)
GLREAL(glPointSize, PFN_glPointSize)
GLREAL(glPolygonOffset, PFN_glPolygonOffset)
GLREAL(glRotatef, PFN_glRotatef)
GLREAL(glSampleCoverage, PFN_glSampleCoverage)
GLREAL(glScalef, PFN_glScalef)
GLREAL(glTexEnvf, PFN_glTexEnvf)
GLREAL(glTexGenfOES, PFN_glTexGenfOES)
GLREAL(glTexParameterf, PFN_glTexParameterf)
GLREAL(glTranslatef, PFN_glTranslatef)

SF void sf_glAlphaFunc(GLenum func, GLfloat ref) {
  void (*r)(GLenum, GLfloat) = real_glAlphaFunc();
  if (r) r(func, ref);
}
SF void sf_glClearColor(GLfloat r0, GLfloat g, GLfloat b, GLfloat a) {
  void (*r)(GLfloat, GLfloat, GLfloat, GLfloat) = real_glClearColor();
  if (r) r(r0, g, b, a);
}
SF void sf_glClearDepthf(GLfloat depth) {
  void (*r)(GLfloat) = real_glClearDepthf();
  if (r) r(depth);
}
SF void sf_glColor4f(GLfloat r0, GLfloat g, GLfloat b, GLfloat a) {
  void (*r)(GLfloat, GLfloat, GLfloat, GLfloat) = real_glColor4f();
  if (r) r(r0, g, b, a);
}
SF void sf_glDepthRangef(GLfloat n, GLfloat f) {
  void (*r)(GLfloat, GLfloat) = real_glDepthRangef();
  if (r) r(n, f);
}
SF void sf_glDrawTexfOES(GLfloat x, GLfloat y, GLfloat z, GLfloat w, GLfloat h) {
  void (*r)(GLfloat, GLfloat, GLfloat, GLfloat, GLfloat) = real_glDrawTexfOES();
  deadspace_gl_prepare_draw("drawtexf", 0, 1);
  if (r) r(x, y, z, w, h);
}
SF void sf_glFogf(GLenum pname, GLfloat param) {
  void (*r)(GLenum, GLfloat) = real_glFogf();
  if (r) r(pname, param);
}
SF void sf_glFrustumf(GLfloat l, GLfloat rgt, GLfloat btm, GLfloat top, GLfloat n, GLfloat f) {
  void (*r)(GLfloat, GLfloat, GLfloat, GLfloat, GLfloat, GLfloat) = real_glFrustumf();
  if (r) r(l, rgt, btm, top, n, f);
}
SF void sf_glLightf(GLenum light, GLenum pname, GLfloat param) {
  void (*r)(GLenum, GLenum, GLfloat) = real_glLightf();
  if (r) r(light, pname, param);
}
SF void sf_glLightModelf(GLenum pname, GLfloat param) {
  void (*r)(GLenum, GLfloat) = real_glLightModelf();
  if (r) r(pname, param);
}
SF void sf_glLineWidth(GLfloat width) {
  void (*r)(GLfloat) = real_glLineWidth();
  if (r) r(width);
}
SF void sf_glMaterialf(GLenum face, GLenum pname, GLfloat param) {
  void (*r)(GLenum, GLenum, GLfloat) = real_glMaterialf();
  if (r) r(face, pname, param);
}
SF void sf_glMultiTexCoord4f(GLenum target, GLfloat s, GLfloat t, GLfloat r0, GLfloat q) {
  void (*r)(GLenum, GLfloat, GLfloat, GLfloat, GLfloat) = real_glMultiTexCoord4f();
  if (r) r(target, s, t, r0, q);
}
SF void sf_glNormal3f(GLfloat nx, GLfloat ny, GLfloat nz) {
  void (*r)(GLfloat, GLfloat, GLfloat) = real_glNormal3f();
  if (r) r(nx, ny, nz);
}
SF void sf_glOrthof(GLfloat l, GLfloat rgt, GLfloat btm, GLfloat top, GLfloat n, GLfloat f) {
  void (*r)(GLfloat, GLfloat, GLfloat, GLfloat, GLfloat, GLfloat) = real_glOrthof();
  if (r) r(l, rgt, btm, top, n, f);
}
SF void sf_glPointParameterf(GLenum pname, GLfloat param) {
  void (*r)(GLenum, GLfloat) = real_glPointParameterf();
  if (r) r(pname, param);
}
SF void sf_glPointSize(GLfloat size) {
  void (*r)(GLfloat) = real_glPointSize();
  if (r) r(size);
}
SF void sf_glPolygonOffset(GLfloat factor, GLfloat units) {
  void (*r)(GLfloat, GLfloat) = real_glPolygonOffset();
  if (r) r(factor, units);
}
SF void sf_glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z) {
  void (*r)(GLfloat, GLfloat, GLfloat, GLfloat) = real_glRotatef();
  if (r) r(angle, x, y, z);
}
SF void sf_glSampleCoverage(GLfloat value, GLboolean invert) {
  void (*r)(GLfloat, GLboolean) = real_glSampleCoverage();
  if (r) r(value, invert);
}
SF void sf_glScalef(GLfloat x, GLfloat y, GLfloat z) {
  void (*r)(GLfloat, GLfloat, GLfloat) = real_glScalef();
  if (r) r(x, y, z);
}
SF void sf_glTexEnvf(GLenum target, GLenum pname, GLfloat param) {
  void (*r)(GLenum, GLenum, GLfloat) = real_glTexEnvf();
  if (r) r(target, pname, param);
}
SF void sf_glTexGenfOES(GLenum coord, GLenum pname, GLfloat param) {
  void (*r)(GLenum, GLenum, GLfloat) = real_glTexGenfOES();
  if (r) r(coord, pname, param);
}
SF void sf_glTexParameterf(GLenum target, GLenum pname, GLfloat param) {
  void (*r)(GLenum, GLenum, GLfloat) = real_glTexParameterf();
  if (r) r(target, pname, param);
}
SF void sf_glTranslatef(GLfloat x, GLfloat y, GLfloat z) {
  void (*r)(GLfloat, GLfloat, GLfloat) = real_glTranslatef();
  if (r) r(x, y, z);
}

struct sfent { const char *name; void *fn; };
static const struct sfent SFTAB[] = {
    {"acos", sf_acos},       {"acosf", sf_acosf},     {"asinf", sf_asinf},
    {"atan", sf_atan},       {"atan2", sf_atan2},     {"atan2f", sf_atan2f},
    {"ceil", sf_ceil},       {"ceilf", sf_ceilf},     {"cos", sf_cos},
    {"cosf", sf_cosf},       {"exp", sf_exp},         {"expf", sf_expf},
    {"floor", sf_floor},     {"floorf", sf_floorf},   {"fmodf", sf_fmodf},
    {"ldexp", sf_ldexp},     {"log", sf_log},         {"log10", sf_log10},
    {"log10f", sf_log10f},   {"logf", sf_logf},       {"modf", sf_modf},
    {"pow", sf_pow},         {"powf", sf_powf},       {"sin", sf_sin},
    {"sinf", sf_sinf},       {"sqrt", sf_sqrt},       {"sqrtf", sf_sqrtf},
    {"strtod", sf_strtod},   {"tan", sf_tan},         {"tanf", sf_tanf},
    {"__isinf", sf___isinf},
    {"glAlphaFunc", sf_glAlphaFunc},
    {"glClearColor", sf_glClearColor},
    {"glClearDepthf", sf_glClearDepthf},
    {"glColor4f", sf_glColor4f},
    {"glDepthRangef", sf_glDepthRangef},
    {"glDrawTexfOES", sf_glDrawTexfOES},
    {"glFogf", sf_glFogf},
    {"glFrustumf", sf_glFrustumf},
    {"glLightf", sf_glLightf},
    {"glLightModelf", sf_glLightModelf},
    {"glLineWidth", sf_glLineWidth},
    {"glMaterialf", sf_glMaterialf},
    {"glMultiTexCoord4f", sf_glMultiTexCoord4f},
    {"glNormal3f", sf_glNormal3f},
    {"glOrthof", sf_glOrthof},
    {"glPointParameterf", sf_glPointParameterf},
    {"glPointSize", sf_glPointSize},
    {"glPolygonOffset", sf_glPolygonOffset},
    {"glRotatef", sf_glRotatef},
    {"glSampleCoverage", sf_glSampleCoverage},
    {"glScalef", sf_glScalef},
    {"glTexEnvf", sf_glTexEnvf},
    {"glTexGenfOES", sf_glTexGenfOES},
    {"glTexParameterf", sf_glTexParameterf},
    {"glTranslatef", sf_glTranslatef},
};

void *softfp_resolve(const char *name) {
  if (!name) return NULL;
  for (unsigned i = 0; i < sizeof(SFTAB) / sizeof(SFTAB[0]); i++)
    if (strcmp(name, SFTAB[i].name) == 0) return SFTAB[i].fn;
  return NULL;
}
