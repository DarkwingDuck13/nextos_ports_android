/*
 * jni_shim.h -- fake JNI environment for Syberia
 *
 * Provides a fake JavaVM and JNIEnv with stub vtables so that
 * JNI calls from libsyberia1.so don't crash.
 */

#ifndef __JNI_SHIM_H__
#define __JNI_SHIM_H__

typedef void (*jni_shim_setpaths_fn)(void *env, void *clazz, void *p0,
                                     void *p1, void *p2);

void jni_shim_init(void **out_vm, void **out_env);

// Set package name and OBB version (call before jni_shim_init)
void jni_shim_set_package(const char *package_name, int obb_version);

// Cria um handle de array JNI fake apontando p/ dados C (int32/float).
// Servido por GetArrayLength/GetIntArrayRegion/GetFloatArrayRegion.
void *jni_shim_make_array(const void *data, int len);

// Cria um jstring fake consumido por GetStringUTFChars/ReleaseStringUTFChars.
void *jni_shim_make_string(const char *value);

// Callback opcional usado quando a engine chama o Java setupPaths().
void jni_shim_set_setpaths_callback(jni_shim_setpaths_fn fn);

#endif
