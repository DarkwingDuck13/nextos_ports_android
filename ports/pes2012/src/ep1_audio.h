#ifndef EP1_AUDIO_H
#define EP1_AUDIO_H

typedef void *(*ep1_make_short_array_fn)(short *data, int len);
typedef void (*ep1_generate_audio_fn)(void *env, void *obj, void *array,
                                      int len);

int ep1_audio_play_apk_mp3(const char *apk_path, int param, long long offset,
                           long long size, int channel);
void ep1_audio_stop(int handle);
void ep1_audio_pause(int handle, int paused);
void ep1_audio_set_volume(int a, int b);
int ep1_audio_is_playing(int handle);
int ep1_audio_status(int handle);
int ep1_audio_duration_ms(int handle);
int ep1_audio_position_ms(int handle);
int ep1_audio_gameplay_music_active(void);
int ep1_audio_menu_music_active(void);
int ep1_audio_title_music_active(void);
void ep1_audio_start_sfx(void *env, void *obj, ep1_generate_audio_fn generate,
                         ep1_make_short_array_fn make_array);
void ep1_audio_stop_sfx(void);

#endif
