/*
 * android_shim.c -- Fake Android NativeActivity/app_glue for Legend of Mana.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "android_shim.h"
#include "util.h"

#define SCREEN_WIDTH 1280
#define SCREEN_HEIGHT 720
#define INPUT_QUEUE_CAP 128

struct AConfiguration {
  int32_t orientation;
  char language[3];
  char country[3];
};

struct ALooper {
  android_app *app;
};

struct AInputEvent {
  int32_t type;
  int32_t source;
  int32_t device_id;
  int32_t action;
  int32_t keycode;
  int32_t meta_state;
  int32_t repeat_count;
  int32_t flags;
  size_t pointer_count;
  int32_t pointer_id;
  float x;
  float y;
  float raw_x;
  float raw_y;
};

struct AInputQueue {
  pthread_mutex_t lock;
  AInputEvent *items[INPUT_QUEUE_CAP];
  int head;
  int tail;
  int pipe_read;
  int pipe_write;
};

typedef struct {
  FILE *fp;
  long size;
  char path[2048];
} FakeAsset;

typedef struct {
  int width;
  int height;
  int format;
  unsigned char pad[8192];
} FakeNativeWindow;

static char g_asset_root[1024] = "payload/assets";
static char g_save_root[1024] = "save";
static FakeNativeWindow g_window = {SCREEN_WIDTH, SCREEN_HEIGHT, 0, {0}};
static android_app *g_app = NULL;
static AInputQueue *g_input_queue = NULL;
static __thread android_app *tls_app = NULL;
static __thread ALooper tls_looper;

static void set_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0)
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void mkdir_if_needed(const char *path) {
  if (path && path[0])
    mkdir(path, 0775);
}

void android_shim_set_asset_root(const char *path) {
  if (!path || !path[0])
    return;
  snprintf(g_asset_root, sizeof(g_asset_root), "%s", path);
}

void android_shim_set_save_root(const char *path) {
  if (!path || !path[0])
    return;
  snprintf(g_save_root, sizeof(g_save_root), "%s", path);
  mkdir_if_needed(g_save_root);
}

static int try_file(const char *path, char *out, size_t out_size) {
  if (!path || !path[0])
    return 0;
  if (access(path, R_OK) == 0) {
    snprintf(out, out_size, "%s", path);
    return 1;
  }
  return 0;
}

static int resolve_asset_file(const char *filename, char *out, size_t out_size) {
  char candidate[2048];
  const char *name = filename;

  if (!filename || !filename[0])
    return 0;

  while (name[0] == '.' && name[1] == '/')
    name += 2;
  if (strncmp(name, "/android_asset/", 15) == 0)
    name += 15;
  if (strncmp(name, "assets/", 7) == 0)
    name += 7;

  if (filename[0] == '/' && try_file(filename, out, out_size))
    return 1;
  if (try_file(filename, out, out_size))
    return 1;

  snprintf(candidate, sizeof(candidate), "%s/%s", g_asset_root, name);
  if (try_file(candidate, out, out_size))
    return 1;

  snprintf(candidate, sizeof(candidate), "%s/assets/%s", g_asset_root, name);
  if (try_file(candidate, out, out_size))
    return 1;

  const char *base = strrchr(name, '/');
  base = base ? base + 1 : name;
  snprintf(candidate, sizeof(candidate), "%s/%s", g_asset_root, base);
  if (try_file(candidate, out, out_size))
    return 1;

  snprintf(candidate, sizeof(candidate), "%s/%s", g_save_root, name);
  if (try_file(candidate, out, out_size))
    return 1;

  snprintf(out, out_size, "%s/%s", g_asset_root, name);
  return 0;
}

void *AAssetManager_fromJava_fake(void *env, void *mgr) {
  (void)env;
  return mgr ? mgr : (void *)0x41415353;
}

void *AAssetManager_open_fake(void *mgr, const char *filename, int mode) {
  (void)mgr;
  (void)mode;

  char path[2048];
  int found = resolve_asset_file(filename, path, sizeof(path));
  debugPrintf("AAssetManager_open: %s -> %s%s\n",
              filename ? filename : "(null)", path, found ? "" : " (missing)");
  if (!found)
    return NULL;

  FILE *fp = fopen(path, "rb");
  if (!fp)
    return NULL;

  FakeAsset *asset = calloc(1, sizeof(*asset));
  if (!asset) {
    fclose(fp);
    return NULL;
  }
  asset->fp = fp;
  snprintf(asset->path, sizeof(asset->path), "%s", path);
  fseek(fp, 0, SEEK_END);
  asset->size = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  return asset;
}

void AAsset_close_fake(void *asset) {
  FakeAsset *a = (FakeAsset *)asset;
  if (!a)
    return;
  if (a->fp)
    fclose(a->fp);
  free(a);
}

int AAsset_read_fake(void *asset, void *buf, size_t count) {
  FakeAsset *a = (FakeAsset *)asset;
  if (!a || !a->fp)
    return -1;
  return (int)fread(buf, 1, count, a->fp);
}

long AAsset_getLength_fake(void *asset) {
  FakeAsset *a = (FakeAsset *)asset;
  return a ? a->size : 0;
}

long AAsset_getRemainingLength_fake(void *asset) {
  FakeAsset *a = (FakeAsset *)asset;
  if (!a || !a->fp)
    return 0;
  long cur = ftell(a->fp);
  return cur >= 0 && a->size >= cur ? a->size - cur : 0;
}

long AAsset_seek_fake(void *asset, long offset, int whence) {
  FakeAsset *a = (FakeAsset *)asset;
  if (!a || !a->fp)
    return -1;
  if (fseek(a->fp, offset, whence) != 0)
    return -1;
  return ftell(a->fp);
}

int AAsset_openFileDescriptor_fake(void *asset, long *outStart,
                                   long *outLength) {
  FakeAsset *a = (FakeAsset *)asset;
  if (!a)
    return -1;
  int fd = open(a->path, O_RDONLY);
  if (fd >= 0) {
    if (outStart)
      *outStart = 0;
    if (outLength)
      *outLength = a->size;
    debugPrintf("AAsset_openFileDescriptor: %s fd=%d len=%ld\n", a->path, fd,
                a->size);
  }
  return fd;
}

void *android_shim_get_native_window(void) { return &g_window; }

void *ANativeWindow_fromSurface_fake(void *env, void *surface) {
  (void)env;
  (void)surface;
  return &g_window;
}

int ANativeWindow_getWidth_fake(void *window) {
  (void)window;
  return g_window.width;
}

int ANativeWindow_getHeight_fake(void *window) {
  (void)window;
  return g_window.height;
}

int ANativeWindow_setBuffersGeometry_fake(void *window, int w, int h, int fmt) {
  (void)window;
  if (w > 0)
    g_window.width = w;
  if (h > 0)
    g_window.height = h;
  g_window.format = fmt;
  debugPrintf("ANativeWindow_setBuffersGeometry(%d,%d,%d)\n", w, h, fmt);
  return 0;
}

int ANativeWindow_setFrameRate_fake(void *window, float frame_rate,
                                    int compatibility) {
  (void)window;
  debugPrintf("ANativeWindow_setFrameRate(%.3f,%d)\n", frame_rate,
              compatibility);
  return 0;
}

AConfiguration *AConfiguration_new_fake(void) {
  AConfiguration *c = calloc(1, sizeof(*c));
  if (c) {
    c->orientation = 2;
    strcpy(c->language, "en");
    strcpy(c->country, "US");
  }
  return c;
}

void AConfiguration_delete_fake(AConfiguration *config) { free(config); }

void AConfiguration_fromAssetManager_fake(AConfiguration *config, void *mgr) {
  (void)config;
  (void)mgr;
}

void AConfiguration_getLanguage_fake(AConfiguration *config, char *outLanguage) {
  (void)config;
  if (outLanguage) {
    outLanguage[0] = 'e';
    outLanguage[1] = 'n';
  }
}

void AConfiguration_getCountry_fake(AConfiguration *config, char *outCountry) {
  (void)config;
  if (outCountry) {
    outCountry[0] = 'U';
    outCountry[1] = 'S';
  }
}

int32_t AConfiguration_getOrientation_fake(AConfiguration *config) {
  return config ? config->orientation : 2;
}

static AInputQueue *input_queue_create(void) {
  AInputQueue *q = calloc(1, sizeof(*q));
  int fds[2];
  if (!q)
    return NULL;
  pthread_mutex_init(&q->lock, NULL);
  if (pipe(fds) == 0) {
    q->pipe_read = fds[0];
    q->pipe_write = fds[1];
    set_nonblock(q->pipe_read);
    set_nonblock(q->pipe_write);
  } else {
    q->pipe_read = -1;
    q->pipe_write = -1;
  }
  return q;
}

static void input_queue_destroy(AInputQueue *q) {
  if (!q)
    return;
  pthread_mutex_lock(&q->lock);
  while (q->head != q->tail) {
    free(q->items[q->head]);
    q->head = (q->head + 1) % INPUT_QUEUE_CAP;
  }
  pthread_mutex_unlock(&q->lock);
  if (q->pipe_read >= 0)
    close(q->pipe_read);
  if (q->pipe_write >= 0)
    close(q->pipe_write);
  pthread_mutex_destroy(&q->lock);
  free(q);
}

static void input_queue_push(AInputEvent *ev) {
  AInputQueue *q = g_input_queue;
  if (!q || !ev) {
    free(ev);
    return;
  }

  pthread_mutex_lock(&q->lock);
  int next = (q->tail + 1) % INPUT_QUEUE_CAP;
  if (next == q->head) {
    free(q->items[q->head]);
    q->head = (q->head + 1) % INPUT_QUEUE_CAP;
  }
  q->items[q->tail] = ev;
  q->tail = next;
  pthread_mutex_unlock(&q->lock);

  if (q->pipe_write >= 0) {
    char b = 1;
    (void)write(q->pipe_write, &b, 1);
  }
}

void android_shim_queue_key(int keycode, int down) {
  AInputEvent *ev = calloc(1, sizeof(*ev));
  if (!ev)
    return;
  ev->type = AINPUT_EVENT_TYPE_KEY;
  ev->source = AINPUT_SOURCE_GAMEPAD | AINPUT_SOURCE_DPAD;
  ev->device_id = 1;
  ev->action = down ? AKEY_EVENT_ACTION_DOWN : AKEY_EVENT_ACTION_UP;
  ev->keycode = keycode;
  input_queue_push(ev);
}

void android_shim_queue_touch(int action, int pointer_id, float x, float y) {
  AInputEvent *ev = calloc(1, sizeof(*ev));
  if (!ev)
    return;
  ev->type = AINPUT_EVENT_TYPE_MOTION;
  ev->source = AINPUT_SOURCE_TOUCHSCREEN;
  ev->device_id = 1;
  ev->action = action;
  ev->pointer_count = 1;
  ev->pointer_id = pointer_id;
  ev->x = x;
  ev->y = y;
  ev->raw_x = x;
  ev->raw_y = y;
  input_queue_push(ev);
}

static void android_process_input(android_app *app, android_poll_source *source) {
  (void)source;
  AInputEvent *event = NULL;
  while (AInputQueue_getEvent_fake(app->inputQueue, &event) >= 0) {
    if (!event)
      break;
    if (AInputQueue_preDispatchEvent_fake(app->inputQueue, event)) {
      continue;
    }
    int handled = 0;
    if (app->onInputEvent)
      handled = app->onInputEvent(app, event);
    AInputQueue_finishEvent_fake(app->inputQueue, event, handled);
  }
}

static int read_cmd(android_app *app, int8_t *cmd) {
  ssize_t n = read(app->msgread, cmd, sizeof(*cmd));
  return n == sizeof(*cmd) ? 1 : 0;
}

static void android_process_cmd(android_app *app, android_poll_source *source) {
  (void)source;
  int8_t cmd = 0;
  if (!read_cmd(app, &cmd))
    return;

  switch (cmd) {
  case APP_CMD_INPUT_CHANGED:
    app->inputQueue = app->pendingInputQueue;
    break;
  case APP_CMD_INIT_WINDOW:
    app->window = app->pendingWindow;
    break;
  case APP_CMD_TERM_WINDOW:
    app->window = NULL;
    break;
  case APP_CMD_CONTENT_RECT_CHANGED:
    app->contentRect = app->pendingContentRect;
    break;
  case APP_CMD_START:
  case APP_CMD_RESUME:
  case APP_CMD_PAUSE:
  case APP_CMD_STOP:
    app->activityState = cmd;
    break;
  case APP_CMD_DESTROY:
    app->destroyRequested = 1;
    break;
  default:
    break;
  }

  debugPrintf("android_app cmd=%d window=%p input=%p\n", cmd, app->window,
              app->inputQueue);
  if (app->onAppCmd)
    app->onAppCmd(app, cmd);
}

android_app *android_shim_create_app(void) {
  android_app *app = calloc(1, sizeof(*app));
  ANativeActivity *act = calloc(1, sizeof(*act));
  int fds[2] = {-1, -1};

  if (!app || !act)
    abort();

  mkdir_if_needed(g_save_root);
  pipe(fds);
  set_nonblock(fds[0]);
  set_nonblock(fds[1]);

  g_input_queue = input_queue_create();

  act->internalDataPath = g_save_root;
  act->externalDataPath = g_save_root;
  act->obbPath = g_asset_root;
  act->sdkVersion = 29;
  act->assetManager = (void *)0x41415353;

  app->activity = act;
  app->config = AConfiguration_new_fake();
  app->msgread = fds[0];
  app->msgwrite = fds[1];
  app->cmdPollSource.id = LOOPER_ID_MAIN;
  app->cmdPollSource.app = app;
  app->cmdPollSource.process = android_process_cmd;
  app->inputPollSource.id = LOOPER_ID_INPUT;
  app->inputPollSource.app = app;
  app->inputPollSource.process = android_process_input;
  app->pendingInputQueue = g_input_queue;
  app->inputQueue = g_input_queue;
  app->pendingWindow = &g_window;
  app->window = NULL;
  app->pendingContentRect.left = 0;
  app->pendingContentRect.top = 0;
  app->pendingContentRect.right = SCREEN_WIDTH;
  app->pendingContentRect.bottom = SCREEN_HEIGHT;
  app->contentRect = app->pendingContentRect;

  g_app = app;
  return app;
}

void android_shim_destroy_app(android_app *app) {
  if (!app)
    return;
  if (app->msgread >= 0)
    close(app->msgread);
  if (app->msgwrite >= 0)
    close(app->msgwrite);
  input_queue_destroy(g_input_queue);
  g_input_queue = NULL;
  AConfiguration_delete_fake(app->config);
  free(app->activity);
  free(app);
  if (g_app == app)
    g_app = NULL;
}

void android_shim_prepare_app_thread(android_app *app) {
  tls_app = app;
  tls_looper.app = app;
  app->looper = &tls_looper;
  ALooper_addFd_fake(app->looper, app->msgread, LOOPER_ID_MAIN, 1, NULL,
                     &app->cmdPollSource);
}

void android_shim_send_cmd(android_app *app, int8_t cmd) {
  if (!app)
    return;
  ssize_t n = write(app->msgwrite, &cmd, sizeof(cmd));
  if (n != sizeof(cmd))
    debugPrintf("android_shim_send_cmd(%d) failed errno=%d\n", cmd, errno);
}

ALooper *ALooper_prepare_fake(int opts) {
  (void)opts;
  tls_looper.app = tls_app ? tls_app : g_app;
  return &tls_looper;
}

int ALooper_addFd_fake(ALooper *looper, int fd, int ident, int events,
                       void *callback, void *data) {
  (void)looper;
  (void)fd;
  (void)ident;
  (void)events;
  (void)callback;
  (void)data;
  return 1;
}

int ALooper_pollAll_fake(int timeoutMillis, int *outFd, int *outEvents,
                         void **outData) {
  android_app *app = tls_app ? tls_app : g_app;
  struct pollfd fds[2];
  int nfds = 0;

  if (outFd)
    *outFd = -1;
  if (outEvents)
    *outEvents = 0;
  if (outData)
    *outData = NULL;
  if (!app)
    return -1;

  fds[nfds].fd = app->msgread;
  fds[nfds].events = POLLIN;
  fds[nfds].revents = 0;
  nfds++;

  if (app->inputQueue && app->inputQueue->pipe_read >= 0) {
    fds[nfds].fd = app->inputQueue->pipe_read;
    fds[nfds].events = POLLIN;
    fds[nfds].revents = 0;
    nfds++;
  }

  int poll_timeout = timeoutMillis;
  if (poll_timeout < 0)
    poll_timeout = 50;
  int ret = poll(fds, nfds, poll_timeout);
  if (ret <= 0)
    return -1;

  if (fds[0].revents & POLLIN) {
    if (outFd)
      *outFd = fds[0].fd;
    if (outEvents)
      *outEvents = fds[0].revents;
    if (outData)
      *outData = &app->cmdPollSource;
    return LOOPER_ID_MAIN;
  }

  if (nfds > 1 && (fds[1].revents & POLLIN)) {
    if (outFd)
      *outFd = fds[1].fd;
    if (outEvents)
      *outEvents = fds[1].revents;
    if (outData)
      *outData = &app->inputPollSource;
    return LOOPER_ID_INPUT;
  }

  return -1;
}

void AInputQueue_attachLooper_fake(AInputQueue *queue, ALooper *looper,
                                   int ident, void *callback, void *data) {
  (void)queue;
  (void)looper;
  (void)ident;
  (void)callback;
  (void)data;
}

void AInputQueue_detachLooper_fake(AInputQueue *queue) { (void)queue; }

int AInputQueue_getEvent_fake(AInputQueue *queue, AInputEvent **outEvent) {
  if (!queue || !outEvent)
    return -1;
  *outEvent = NULL;

  if (queue->pipe_read >= 0) {
    char b;
    (void)read(queue->pipe_read, &b, 1);
  }

  pthread_mutex_lock(&queue->lock);
  if (queue->head != queue->tail) {
    *outEvent = queue->items[queue->head];
    queue->items[queue->head] = NULL;
    queue->head = (queue->head + 1) % INPUT_QUEUE_CAP;
  }
  pthread_mutex_unlock(&queue->lock);
  return *outEvent ? 0 : -1;
}

int AInputQueue_preDispatchEvent_fake(AInputQueue *queue, AInputEvent *event) {
  (void)queue;
  (void)event;
  return 0;
}

void AInputQueue_finishEvent_fake(AInputQueue *queue, AInputEvent *event,
                                  int handled) {
  (void)queue;
  (void)handled;
  free(event);
}

int32_t AInputEvent_getType_fake(const AInputEvent *event) {
  return event ? event->type : 0;
}

int32_t AInputEvent_getDeviceId_fake(const AInputEvent *event) {
  return event ? event->device_id : 0;
}

int32_t AInputEvent_getSource_fake(const AInputEvent *event) {
  return event ? event->source : 0;
}

int32_t AKeyEvent_getAction_fake(const AInputEvent *event) {
  return event ? event->action : 0;
}

int32_t AKeyEvent_getKeyCode_fake(const AInputEvent *event) {
  return event ? event->keycode : 0;
}

int32_t AKeyEvent_getMetaState_fake(const AInputEvent *event) {
  return event ? event->meta_state : 0;
}

int32_t AKeyEvent_getRepeatCount_fake(const AInputEvent *event) {
  return event ? event->repeat_count : 0;
}

int32_t AMotionEvent_getAction_fake(const AInputEvent *event) {
  return event ? event->action : 0;
}

int32_t AMotionEvent_getFlags_fake(const AInputEvent *event) {
  return event ? event->flags : 0;
}

size_t AMotionEvent_getPointerCount_fake(const AInputEvent *event) {
  return event && event->pointer_count ? event->pointer_count : 1;
}

int32_t AMotionEvent_getPointerId_fake(const AInputEvent *event, size_t idx) {
  (void)idx;
  return event ? event->pointer_id : 0;
}

float AMotionEvent_getX_fake(const AInputEvent *event, size_t idx) {
  (void)idx;
  return event ? event->x : 0.0f;
}

float AMotionEvent_getY_fake(const AInputEvent *event, size_t idx) {
  (void)idx;
  return event ? event->y : 0.0f;
}

float AMotionEvent_getRawX_fake(const AInputEvent *event, size_t idx) {
  (void)idx;
  return event ? event->raw_x : 0.0f;
}

float AMotionEvent_getRawY_fake(const AInputEvent *event, size_t idx) {
  (void)idx;
  return event ? event->raw_y : 0.0f;
}

float AMotionEvent_getAxisValue_fake(const AInputEvent *event, int32_t axis,
                                     size_t idx) {
  (void)event;
  (void)axis;
  (void)idx;
  return 0.0f;
}

void ANativeActivity_finish_fake(ANativeActivity *activity) {
  (void)activity;
  if (g_app)
    g_app->destroyRequested = 1;
}

void *ASensorManager_getInstance_fake(void) { return NULL; }

void *ASensorManager_getDefaultSensor_fake(void *manager, int type) {
  (void)manager;
  (void)type;
  return NULL;
}

void *ASensorManager_createEventQueue_fake(void *manager, ALooper *looper,
                                           int ident, void *callback,
                                           void *data) {
  (void)manager;
  (void)looper;
  (void)ident;
  (void)callback;
  (void)data;
  return NULL;
}

int ASensorEventQueue_enableSensor_fake(void *queue, void *sensor) {
  (void)queue;
  (void)sensor;
  return 0;
}

int ASensorEventQueue_disableSensor_fake(void *queue, void *sensor) {
  (void)queue;
  (void)sensor;
  return 0;
}

int ASensorEventQueue_setEventRate_fake(void *queue, void *sensor,
                                        int32_t usec) {
  (void)queue;
  (void)sensor;
  (void)usec;
  return 0;
}

ssize_t ASensorEventQueue_getEvents_fake(void *queue, void *events,
                                         size_t count) {
  (void)queue;
  (void)events;
  (void)count;
  return 0;
}
