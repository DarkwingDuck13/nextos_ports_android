#ifndef DEADSPACE_JNI_SHIM_H
#define DEADSPACE_JNI_SHIM_H

#include <stddef.h>
#include <stdint.h>

void jni_shim_init(void **out_vm, void **out_env);
void jni_set_asset_root(const char *path);
void jni_set_display_size(int w, int h);
void jni_set_audio_output(void (*write_cb)(const int16_t *samples, int sample_count),
                          void (*state_cb)(int playing));

void *jni_asset_manager(void);
void *jni_activity_object(void);
void *jni_renderer_object(void);
void *jni_audio_track_object(void);
void *jni_make_string(const char *value);

unsigned char *jni_shim_get_array(void *handle, int *out_len);

#endif
