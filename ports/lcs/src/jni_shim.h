#ifndef JNI_SHIM_H
#define JNI_SHIM_H
void jni_load(void);
void jni_init_input(void);
void *NVThreadGetCurrentJNIEnv(void);
int LCS_OS_GamepadIsConnected(unsigned int pad, void *type);
float LCS_OS_GamepadAxis(unsigned int pad, unsigned int axis);
#endif
