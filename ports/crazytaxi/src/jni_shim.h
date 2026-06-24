#ifndef __JNI_SHIM_H__
#define __JNI_SHIM_H__

void jni_shim_init(void **out_vm, void **out_env);

void* AAssetManager_fromJava(void* env, void* assetManager);

#endif
