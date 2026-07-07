#ifndef __JNI_SHIM_H__
#define __JNI_SHIM_H__

void jni_shim_init(void **out_vm, void **out_env);
void *jni_make_string(const char *value);

void *kof_jni_int_array(const int *values, int len);
void *kof_jni_float_array(const float *values, int len);

void kof_jni_key_down(int mask);
void kof_jni_key_up(int mask);
void kof_jni_set_key_state(int state);
void kof_jni_clear_key_trigger(void);
int kof_jni_key_state(void);
int kof_jni_key_trigger(void);
int kof_jni_text_draw_serial(void);

const char *kof_jni_movie_name(void);
int kof_jni_movie_state(void);
int kof_jni_movie_position_ms(void);
int kof_jni_movie_duration_ms(void);
int kof_jni_movie_is_playing(void);
void kof_jni_movie_mark_finished(void);

#endif
