#ifndef __JNI_SHIM_H__
#define __JNI_SHIM_H__

void jni_shim_init(void **out_vm, void **out_env);

/* Cria um jstring falso que GetStringUTFChars resolve de volta para 'value'. */
void *jni_make_string(const char *value);

void *AAssetManager_fromJava(void *env, void *assetManager);

/* ---- Estado de input do plandroid (PlAndroidSensor) ----
 * main.c preenche a partir do SDL; a upcall GetSensorStateFunc([I) copia
 * para o int[37] que o engine (main_OnFrame) le por frame. */
typedef struct {
  int key_now, key_last, key_on, key_off;  /* bitmask de botoes */
  int touch_now_b, touch_last_b, touch_on_b, touch_off_b, touch_moving_b, touch_move_b;
  int touch_ptr_max, touch_count, touch_last_ptr;
  int touch_max_x, touch_max_y;
  int touch_start_x[4], touch_start_y[4];
  int touch_move_x[4], touch_move_y[4];
  int analog_x[2], analog_y[2];
} som_input_t;
extern som_input_t g_som_input;

/* idioma forcado (enum plandroid; default via SOM_LANG). */
extern int g_som_lang;

#endif
