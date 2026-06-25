/* inject.c — injetor uinput de TECLADO p/ testar input do port sem humano.
 * Cria /dev/uinput keyboard, espera delay inicial (SDL enumera no boot do jogo),
 * depois reproduz sequencia de teclas passada por argv: "KEY:ms KEY:ms ...".
 * KEY = nome curto (UP DOWN LEFT RIGHT ENTER ESC Z X C V SPACE SHIFT).
 * Build: aarch64 toolchain. Uso: ./inject <delay_inicial_ms> KEY:ms KEY:ms ...
 */
#include <linux/uinput.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static int kc(const char *n) {
  if (!strcmp(n, "UP")) return KEY_UP;
  if (!strcmp(n, "DOWN")) return KEY_DOWN;
  if (!strcmp(n, "LEFT")) return KEY_LEFT;
  if (!strcmp(n, "RIGHT")) return KEY_RIGHT;
  if (!strcmp(n, "ENTER")) return KEY_ENTER;
  if (!strcmp(n, "ESC")) return KEY_ESC;
  if (!strcmp(n, "Z")) return KEY_Z;
  if (!strcmp(n, "X")) return KEY_X;
  if (!strcmp(n, "C")) return KEY_C;
  if (!strcmp(n, "V")) return KEY_V;
  if (!strcmp(n, "SPACE")) return KEY_SPACE;
  if (!strcmp(n, "SHIFT")) return KEY_LEFTSHIFT;
  return -1;
}

static void emit(int fd, int type, int code, int val) {
  struct input_event ev; memset(&ev, 0, sizeof ev);
  ev.type = type; ev.code = code; ev.value = val;
  write(fd, &ev, sizeof ev);
}
static void key(int fd, int code) {
  emit(fd, EV_KEY, code, 1); emit(fd, EV_SYN, SYN_REPORT, 0);
  usleep(40000);
  emit(fd, EV_KEY, code, 0); emit(fd, EV_SYN, SYN_REPORT, 0);
}

int main(int argc, char **argv) {
  int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
  if (fd < 0) { perror("open uinput"); return 1; }
  ioctl(fd, UI_SET_EVBIT, EV_KEY);
  int keys[] = {KEY_UP,KEY_DOWN,KEY_LEFT,KEY_RIGHT,KEY_ENTER,KEY_ESC,
                KEY_Z,KEY_X,KEY_C,KEY_V,KEY_SPACE,KEY_LEFTSHIFT};
  for (unsigned i = 0; i < sizeof(keys)/sizeof(keys[0]); i++)
    ioctl(fd, UI_SET_KEYBIT, keys[i]);
  struct uinput_user_dev uidev; memset(&uidev, 0, sizeof uidev);
  snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "kz-inject-kbd");
  uidev.id.bustype = BUS_USB; uidev.id.vendor = 0x1; uidev.id.product = 0x1; uidev.id.version = 1;
  write(fd, &uidev, sizeof uidev);
  ioctl(fd, UI_DEV_CREATE);

  int delay = (argc > 1) ? atoi(argv[1]) : 6000;
  fprintf(stderr, "[inject] device criado, esperando %dms p/ SDL enumerar...\n", delay);
  usleep(delay * 1000);

  for (int i = 2; i < argc; i++) {
    char buf[64]; strncpy(buf, argv[i], 63); buf[63] = 0;
    char *colon = strchr(buf, ':');
    int ms = 300;
    if (colon) { *colon = 0; ms = atoi(colon + 1); }
    int code = kc(buf);
    if (code < 0) { fprintf(stderr, "[inject] tecla desconhecida: %s\n", buf); continue; }
    fprintf(stderr, "[inject] %s (wait %dms)\n", buf, ms);
    key(fd, code);
    usleep(ms * 1000);
  }
  fprintf(stderr, "[inject] sequencia completa, mantendo device 3s\n");
  usleep(3000000);
  ioctl(fd, UI_DEV_DESTROY);
  close(fd);
  return 0;
}
