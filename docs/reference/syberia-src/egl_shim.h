/*
 * egl_shim.h -- EGL wrapper backed by SDL2
 *
 * libsyberia1.so manages its own EGL context. We intercept EGL calls
 * and route them through SDL2's OpenGL ES support.
 */

#ifndef __EGL_SHIM_H__
#define __EGL_SHIM_H__

#include <stdint.h>

// EGL types
typedef void *EGLDisplay;
typedef void *EGLSurface;
typedef void *EGLContext;
typedef void *EGLConfig;
typedef void *EGLNativeDisplayType;
typedef void *EGLNativeWindowType;
typedef unsigned int EGLBoolean;
typedef int32_t EGLint;

#define EGL_TRUE 1
#define EGL_FALSE 0
#define EGL_SUCCESS 0x3000
#define EGL_DEFAULT_DISPLAY ((EGLNativeDisplayType)0)
#define EGL_NO_DISPLAY ((EGLDisplay)0)
#define EGL_NO_CONTEXT ((EGLContext)0)
#define EGL_NO_SURFACE ((EGLSurface)0)

// Our shim functions (match EGL API signatures)
EGLDisplay egl_shim_GetDisplay(EGLNativeDisplayType display_id);
EGLBoolean egl_shim_Initialize(EGLDisplay dpy, EGLint *major, EGLint *minor);
EGLBoolean egl_shim_Terminate(EGLDisplay dpy);
EGLBoolean egl_shim_ChooseConfig(EGLDisplay dpy, const EGLint *attrib_list,
                                  EGLConfig *configs, EGLint config_size,
                                  EGLint *num_config);
EGLSurface egl_shim_CreateWindowSurface(EGLDisplay dpy, EGLConfig config,
                                         EGLNativeWindowType win,
                                         const EGLint *attrib_list);
EGLContext egl_shim_CreateContext(EGLDisplay dpy, EGLConfig config,
                                  EGLContext share_context,
                                  const EGLint *attrib_list);
EGLBoolean egl_shim_MakeCurrent(EGLDisplay dpy, EGLSurface draw,
                                 EGLSurface read, EGLContext ctx);
EGLBoolean egl_shim_SwapBuffers(EGLDisplay dpy, EGLSurface surface);
EGLBoolean egl_shim_DestroySurface(EGLDisplay dpy, EGLSurface surface);
EGLBoolean egl_shim_DestroyContext(EGLDisplay dpy, EGLContext ctx);
EGLBoolean egl_shim_QuerySurface(EGLDisplay dpy, EGLSurface surface,
                                  EGLint attribute, EGLint *value);
EGLint egl_shim_GetError(void);
void *egl_shim_GetProcAddress(const char *procname);

#endif
