/*
 * jni_shim.h -- fake JNI environment for Syberia
 *
 * Provides a fake JavaVM and JNIEnv with stub vtables so that
 * JNI calls from libsyberia1.so don't crash.
 */

#ifndef __JNI_SHIM_H__
#define __JNI_SHIM_H__

struct hk_inject_s {
  int action;
  int keycode;
  int source;
  int deviceId;
  int metaState;
  int repeat;
  int scancode;
  int flags;
  int unicode;
  long eventTime;
  long downTime;
  float axes[64];   /* MotionEvent: eixos analogicos (getAxisValue), indexados por AXIS_* */
};

void jni_shim_init(void **out_vm, void **out_env);

// recon: acha o ponteiro de um metodo nativo registrado via RegisterNatives
void *jni_find_native(const char *name);

// Set package name and OBB version (call before jni_shim_init)
void jni_shim_set_package(const char *package_name, int obb_version);

extern struct hk_inject_s g_hk_inject;
void *hk_keyevent_object(void);
void *hk_motionevent_object(void);  /* sentinela do MotionEvent injetado (stick analogico) */

/* FMOD AudioTrack output (org.fmod.FMODAudioDevice) — usado pela fmod_audio_thread */
void *jni_fmod_device(void);
void *jni_fmod_bytebuffer(void);
void *jni_fmod_pcm(void);
int jni_fmod_pcm_size(void);
extern int g_fmod_cap;

#endif
