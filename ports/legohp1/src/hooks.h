/* hooks.h -- LEGO Harry Potter (libLEGOHarry.so) hook surface (GLES1)
 *
 * Classic GLSurfaceView model: the wrapper owns display/window/context/swap via
 * SDL2 (GLES1). GL calls bind straight to libGLESv1_CM (imports.c); only
 * glGetString is wrapped (engine deref's the result). The single-context
 * handover machinery survives as no-op stubs so pthr.c / libc_shim.c compile.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __HOOKS_H__
#define __HOOKS_H__

#include <GLES/gl.h>

void patch_game(void);

// --- wrapper-owned EGL (egl.c) ---------------------------------------------
int  egl_bringup(void);
void egl_present(void);
void egl_fbo_frame_summary(unsigned frame);
void egl_resize_surface(int w, int h);
void *egl_display(void);
void egl_mark_render_thread(void);

// --- GL-ownership stubs (no-ops; kept for pthr.c / libc_shim.c) -------------
void egl_gl_ownership_park(void);
void egl_gl_ownership_release(void);
void egl_gl_service_handover(void);
void egl_gl_acquire(void);
int  egl_gl_thread_holds_context(void);

extern volatile int egl_swap_count;
extern volatile unsigned long long egl_last_compile_tick;

// --- EGL hooks bound to the game's imports (egl.c) --------------------------
unsigned eglBindAPIHook(unsigned api);
void    *eglCreateContextHook(void *dpy, void *config, void *share, const int *attrib_list);
void    *eglCreatePbufferSurfaceHook(void *dpy, void *config, const int *attrib_list);
unsigned eglMakeCurrentHook(void *dpy, void *draw, void *read, void *ctx);
unsigned eglSwapBuffersHook(void *dpy, void *surface);
unsigned eglSwapIntervalHook(void *dpy, int interval);
unsigned eglGetConfigAttribHook(void *dpy, void *config, int attribute, int *value);

const GLubyte *glGetStringHook(GLenum name);

#endif
