/*
 * jni_shim.h — fake JNIEnv/JavaVM para o Karisma engine (BFBC2, com.dle.bc2).
 *
 * O engine é JNI-driven: durante createContext/render ele chama de volta os
 * métodos static de com.dle.bc2.KarismaBridge (OpenResource, GetSliderState,
 * device queries, controle de som) e lê os campos static mWidth/mHeight. Aqui
 * montamos vtables JNI que respondem a essas up-calls. Sem Java real.
 */
#ifndef BFBC2_JNI_SHIM_H
#define BFBC2_JNI_SHIM_H

void jni_shim_init(void **out_vm, void **out_env);

/* tela conhecida pelo engine (setada antes do createContext, = SetWidthHeightScreen) */
void jni_shim_set_screen(int w, int h);

/* pasta com os assets extraídos do APK (OpenResource lê daqui por prefixo) */
void jni_shim_set_assets_dir(const char *dir);

/* fabrica um jshortArray fake apontando pro buffer real de áudio (updateSound) */
void *jni_make_short_array(short *data, int len);

/* controle de som pedido pelo engine (Enable/Disable/Lock/Unlock) — lido pelo main */
extern volatile int g_snd_enabled;   /* EnableSound=1 / DisableSound=0 */
extern volatile int g_snd_locked;    /* LockSound=1 / UnlockSound=0 */
extern volatile int g_want_exit;     /* ExitApplication */

#endif
