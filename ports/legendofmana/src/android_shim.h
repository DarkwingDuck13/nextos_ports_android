/*
 * android_shim.h -- Fake Android NativeActivity/app_glue for Legend of Mana.
 */

#ifndef __ANDROID_SHIM_H__
#define __ANDROID_SHIM_H__

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct ARect {
  int32_t left;
  int32_t top;
  int32_t right;
  int32_t bottom;
} ARect;

typedef struct ANativeActivityCallbacks ANativeActivityCallbacks;
typedef struct ANativeActivity {
  ANativeActivityCallbacks *callbacks;
  void *vm;
  void *env;
  void *clazz;
  const char *internalDataPath;
  const char *externalDataPath;
  int32_t sdkVersion;
  void *instance;
  void *assetManager;
  const char *obbPath;
} ANativeActivity;

typedef struct AConfiguration AConfiguration;
typedef struct AInputQueue AInputQueue;
typedef struct AInputEvent AInputEvent;
typedef struct ALooper ALooper;

struct android_app;

typedef struct android_poll_source {
  int32_t id;
  struct android_app *app;
  void (*process)(struct android_app *app, struct android_poll_source *source);
} android_poll_source;

typedef struct android_app {
  void *userData;
  void (*onAppCmd)(struct android_app *app, int32_t cmd);
  int32_t (*onInputEvent)(struct android_app *app, AInputEvent *event);
  ANativeActivity *activity;
  AConfiguration *config;
  void *savedState;
  size_t savedStateSize;
  ALooper *looper;
  AInputQueue *inputQueue;
  void *window;
  ARect contentRect;
  int activityState;
  int destroyRequested;
  void *mutex_pad[8];
  void *cond_pad[8];
  int msgread;
  int msgwrite;
  unsigned long thread;
  android_poll_source cmdPollSource;
  android_poll_source inputPollSource;
  int running;
  int stateSaved;
  int destroyed;
  int redrawNeeded;
  AInputQueue *pendingInputQueue;
  void *pendingWindow;
  ARect pendingContentRect;
} android_app;

enum {
  APP_CMD_INPUT_CHANGED = 0,
  APP_CMD_INIT_WINDOW = 1,
  APP_CMD_TERM_WINDOW = 2,
  APP_CMD_WINDOW_RESIZED = 3,
  APP_CMD_WINDOW_REDRAW_NEEDED = 4,
  APP_CMD_CONTENT_RECT_CHANGED = 5,
  APP_CMD_GAINED_FOCUS = 6,
  APP_CMD_LOST_FOCUS = 7,
  APP_CMD_CONFIG_CHANGED = 8,
  APP_CMD_LOW_MEMORY = 9,
  APP_CMD_START = 10,
  APP_CMD_RESUME = 11,
  APP_CMD_SAVE_STATE = 12,
  APP_CMD_PAUSE = 13,
  APP_CMD_STOP = 14,
  APP_CMD_DESTROY = 15
};

enum {
  LOOPER_ID_MAIN = 1,
  LOOPER_ID_INPUT = 2,
  LOOPER_ID_USER = 3
};

#define AINPUT_EVENT_TYPE_KEY 1
#define AINPUT_EVENT_TYPE_MOTION 2

#define AKEY_EVENT_ACTION_DOWN 0
#define AKEY_EVENT_ACTION_UP 1

#define AMOTION_EVENT_ACTION_DOWN 0
#define AMOTION_EVENT_ACTION_UP 1
#define AMOTION_EVENT_ACTION_MOVE 2

#define AINPUT_SOURCE_KEYBOARD 0x00000101
#define AINPUT_SOURCE_DPAD 0x00000201
#define AINPUT_SOURCE_GAMEPAD 0x00000401
#define AINPUT_SOURCE_TOUCHSCREEN 0x00001002
#define AINPUT_SOURCE_JOYSTICK 0x01000010

android_app *android_shim_create_app(void);
void android_shim_destroy_app(android_app *app);
void android_shim_prepare_app_thread(android_app *app);
void android_shim_send_cmd(android_app *app, int8_t cmd);
void android_shim_set_asset_root(const char *path);
void android_shim_set_save_root(const char *path);
void android_shim_queue_key(int keycode, int down);
void android_shim_queue_touch(int action, int pointer_id, float x, float y);
void *android_shim_get_native_window(void);

/* AAssetManager */
void *AAssetManager_fromJava_fake(void *env, void *mgr);
void *AAssetManager_open_fake(void *mgr, const char *filename, int mode);
void AAsset_close_fake(void *asset);
int AAsset_read_fake(void *asset, void *buf, size_t count);
long AAsset_getLength_fake(void *asset);
long AAsset_getRemainingLength_fake(void *asset);
long AAsset_seek_fake(void *asset, long offset, int whence);
int AAsset_openFileDescriptor_fake(void *asset, long *outStart, long *outLength);

/* ANativeWindow */
void *ANativeWindow_fromSurface_fake(void *env, void *surface);
int ANativeWindow_getWidth_fake(void *window);
int ANativeWindow_getHeight_fake(void *window);
int ANativeWindow_setBuffersGeometry_fake(void *window, int w, int h, int fmt);
int ANativeWindow_setFrameRate_fake(void *window, float frame_rate,
                                    int compatibility);

/* Android configuration */
AConfiguration *AConfiguration_new_fake(void);
void AConfiguration_delete_fake(AConfiguration *config);
void AConfiguration_fromAssetManager_fake(AConfiguration *config, void *mgr);
void AConfiguration_getLanguage_fake(AConfiguration *config, char *outLanguage);
void AConfiguration_getCountry_fake(AConfiguration *config, char *outCountry);
int32_t AConfiguration_getOrientation_fake(AConfiguration *config);

/* Looper/input queue */
ALooper *ALooper_prepare_fake(int opts);
int ALooper_addFd_fake(ALooper *looper, int fd, int ident, int events,
                       void *callback, void *data);
int ALooper_pollAll_fake(int timeoutMillis, int *outFd, int *outEvents,
                         void **outData);
void AInputQueue_attachLooper_fake(AInputQueue *queue, ALooper *looper,
                                   int ident, void *callback, void *data);
void AInputQueue_detachLooper_fake(AInputQueue *queue);
int AInputQueue_getEvent_fake(AInputQueue *queue, AInputEvent **outEvent);
int AInputQueue_preDispatchEvent_fake(AInputQueue *queue, AInputEvent *event);
void AInputQueue_finishEvent_fake(AInputQueue *queue, AInputEvent *event,
                                  int handled);
int32_t AInputEvent_getType_fake(const AInputEvent *event);
int32_t AInputEvent_getDeviceId_fake(const AInputEvent *event);
int32_t AInputEvent_getSource_fake(const AInputEvent *event);
int32_t AKeyEvent_getAction_fake(const AInputEvent *event);
int32_t AKeyEvent_getKeyCode_fake(const AInputEvent *event);
int32_t AKeyEvent_getMetaState_fake(const AInputEvent *event);
int32_t AKeyEvent_getRepeatCount_fake(const AInputEvent *event);
int32_t AMotionEvent_getAction_fake(const AInputEvent *event);
int32_t AMotionEvent_getFlags_fake(const AInputEvent *event);
size_t AMotionEvent_getPointerCount_fake(const AInputEvent *event);
int32_t AMotionEvent_getPointerId_fake(const AInputEvent *event, size_t idx);
float AMotionEvent_getX_fake(const AInputEvent *event, size_t idx);
float AMotionEvent_getY_fake(const AInputEvent *event, size_t idx);
float AMotionEvent_getRawX_fake(const AInputEvent *event, size_t idx);
float AMotionEvent_getRawY_fake(const AInputEvent *event, size_t idx);
float AMotionEvent_getAxisValue_fake(const AInputEvent *event, int32_t axis,
                                     size_t idx);

/* NativeActivity/sensors */
void ANativeActivity_finish_fake(ANativeActivity *activity);
void *ASensorManager_getInstance_fake(void);
void *ASensorManager_getDefaultSensor_fake(void *manager, int type);
void *ASensorManager_createEventQueue_fake(void *manager, ALooper *looper,
                                           int ident, void *callback,
                                           void *data);
int ASensorEventQueue_enableSensor_fake(void *queue, void *sensor);
int ASensorEventQueue_disableSensor_fake(void *queue, void *sensor);
int ASensorEventQueue_setEventRate_fake(void *queue, void *sensor,
                                        int32_t usec);
ssize_t ASensorEventQueue_getEvents_fake(void *queue, void *events,
                                         size_t count);

#endif
