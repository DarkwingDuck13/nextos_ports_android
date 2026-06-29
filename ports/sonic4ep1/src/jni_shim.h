/*
 * jni_shim.h -- fake JNI environment for Syberia
 *
 * Provides a fake JavaVM and JNIEnv with stub vtables so that
 * JNI calls from libsyberia1.so don't crash.
 */

#ifndef __JNI_SHIM_H__
#define __JNI_SHIM_H__

void jni_shim_init(void **out_vm, void **out_env);

// Recon: metodos nativos registrados via RegisterNatives no JNI_OnLoad.
void *jni_find_native(const char *name);
void *jni_shim_os_tick_fn(void);
void jni_dump_natives(void);
void *jni_shim_new_string(const char *value);
void *jni_shim_make_array(const void *data, int len);
void *jni_shim_make_short_array(short *data, int len);

// Set package name and OBB version (call before jni_shim_init)
void jni_shim_set_package(const char *package_name, int obb_version);

#endif
