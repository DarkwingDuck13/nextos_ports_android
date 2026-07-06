#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "util.h"

#define LOG_NAME "debug.log"

int debugPrintf(const char *text, ...) {
  va_list list;

  FILE *f = fopen(LOG_NAME, "a");
  if (f) {
    va_start(list, text);
    vfprintf(f, text, list);
    va_end(list);
    fclose(f);
  }

  va_start(list, text);
  vprintf(text, list);
  va_end(list);

  return 0;
}

uintptr_t read_tls_stack_guard(void) {
#if defined(__aarch64__)
  uintptr_t tls = 0;
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tls));
  if (tls)
    return *(uintptr_t *)(tls + 0x28);
#endif
  return 0;
}

static void strip_hd_suffix_local(char *path) {
  char *dot = strrchr(path, '.');
  if (dot && dot - path >= 3 && memcmp(dot - 3, "-hd", 3) == 0)
    memmove(dot - 3, dot, strlen(dot) + 1);
}

static const char *try_asset_candidate(char *out, size_t out_size,
                                       const char *relative) {
  if (snprintf(out, out_size, "./assets/%s", relative) >= (int)out_size)
    return NULL;
  if (access(out, F_OK) == 0)
    return out;

  char no_hd[2048];
  snprintf(no_hd, sizeof(no_hd), "%s", out);
  strip_hd_suffix_local(no_hd);
  if (strcmp(no_hd, out) != 0 && access(no_hd, F_OK) == 0) {
    snprintf(out, out_size, "%s", no_hd);
    return out;
  }
  return NULL;
}

static const char *resolve_graphics_alias(char *out, size_t out_size,
                                          const char *path) {
  const char *relative = path;
  if (strncmp(relative, "./assets/", 9) == 0)
    relative += 9;
  else if (strncmp(relative, "assets/", 7) == 0)
    relative += 7;

  const char *suffix = NULL;
  const char *bases[2] = {NULL, NULL};
  if (strncmp(relative, "graphics/sd_etc2/", 17) == 0) {
    suffix = relative + 17;
    bases[0] = "graphics/sd_common";
    bases[1] = "graphics/sd";
  } else if (strncmp(relative, "graphics/hd_etc2/", 17) == 0) {
    suffix = relative + 17;
    bases[0] = "graphics/sd_common";
    bases[1] = "graphics/sd";
  } else if (strncmp(relative, "graphics/hd_common/", 19) == 0) {
    suffix = relative + 19;
    bases[0] = "graphics/sd_common";
  } else if (strncmp(relative, "graphics/hd/", 12) == 0) {
    suffix = relative + 12;
    bases[0] = "graphics/sd";
  }
  if (!suffix)
    return NULL;

  for (int i = 0; i < 2 && bases[i]; i++) {
    char candidate[2048];
    snprintf(candidate, sizeof(candidate), "%s/%s", bases[i], suffix);
    const char *resolved = try_asset_candidate(out, out_size, candidate);
    if (resolved)
      return resolved;
  }
  return NULL;
}

static const char *resolve_graphics_relative(char *out, size_t out_size,
                                             const char *path) {
  if (path[0] == '/' || strncmp(path, "graphics/", 9) == 0 ||
      strncmp(path, "./", 2) == 0 || strncmp(path, "../", 3) == 0)
    return NULL;
  if (!strstr(path, ".pvr") && !strstr(path, ".plist"))
    return NULL;

  static const char *sd_dirs[] = {
      "graphics/sd_common",
      "graphics/sd",
      "graphics/hd_common",
      "graphics/hd",
  };
  for (size_t i = 0; i < sizeof(sd_dirs) / sizeof(sd_dirs[0]); i++) {
    char candidate[2048];
    snprintf(candidate, sizeof(candidate), "%s/%s", sd_dirs[i], path);
    const char *resolved = try_asset_candidate(out, out_size, candidate);
    if (resolved)
      return resolved;
  }
  return NULL;
}

const char *resolve_android_path(const char *path) {
  static _Thread_local char alt_path[2048];
  static _Thread_local char asset_path[2048];

  static const char *app_data_prefix = "/data/data/com.sega.CrazyTaxi/";
  static const char *app_data_prefix_2 = "/data/user/0/com.sega.CrazyTaxi/";
  static const char *sdcard_prefix =
      "/storage/emulated/0/Android/data/com.sega.CrazyTaxi/";

  if (!path || path[0] == '\0')
    return path;

  const char *alias = resolve_graphics_alias(asset_path, sizeof(asset_path), path);
  if (alias)
    return alias;

  alias = resolve_graphics_relative(asset_path, sizeof(asset_path), path);
  if (alias)
    return alias;

  if (strncmp(path, app_data_prefix, strlen(app_data_prefix)) == 0) {
    snprintf(alt_path, sizeof(alt_path), "./%s",
             path + strlen(app_data_prefix));
    return alt_path;
  }
  if (strncmp(path, app_data_prefix_2, strlen(app_data_prefix_2)) == 0) {
    snprintf(alt_path, sizeof(alt_path), "./%s",
             path + strlen(app_data_prefix_2));
    return alt_path;
  }
  if (strncmp(path, sdcard_prefix, strlen(sdcard_prefix)) == 0) {
    snprintf(alt_path, sizeof(alt_path), "./%s", path + strlen(sdcard_prefix));
    return alt_path;
  }

  if (access(path, F_OK) == 0)
    return path;

  if (path[0] == '/') {
    if (snprintf(alt_path, sizeof(alt_path), ".%s", path) <
            (int)sizeof(alt_path) &&
        access(alt_path, F_OK) == 0) {
      return alt_path;
    }
  }

  const char *basename = strrchr(path, '/');
  basename = basename ? basename + 1 : path;
  if (basename[0] != '\0') {
    if (snprintf(asset_path, sizeof(asset_path), "./assets/%s", basename) <
            (int)sizeof(asset_path) &&
        access(asset_path, F_OK) == 0) {
      return asset_path;
    }
  }

  return path;
}

int ret0(void) { return 0; }
int ret1(void) { return 1; }
int retm1(void) { return -1; }
