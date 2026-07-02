/* vgpad: gamepad virtual via uinput p/ testar jogos (gira camera/anda).
 * uso: ./vgpad rx <val -32767..32767> <ms>   (analogico direito X)
 *      ./vgpad ry <val> <ms> | lx | ly
 * cria o pad, segura o eixo pelo tempo, centraliza e sai. */
#include <fcntl.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void emit(int fd, int type, int code, int val) {
  struct input_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = type;
  ev.code = code;
  ev.value = val;
  write(fd, &ev, sizeof(ev));
}
static void syn(int fd) { emit(fd, EV_SYN, SYN_REPORT, 0); }

int main(int argc, char **argv) {
  if (argc < 4) {
    fprintf(stderr, "uso: %s rx|ry|lx|ly <val> <ms>\n", argv[0]);
    return 1;
  }
  int axis = ABS_RX;
  if (!strcmp(argv[1], "rx")) axis = ABS_RX;
  else if (!strcmp(argv[1], "ry")) axis = ABS_RY;
  else if (!strcmp(argv[1], "lx")) axis = ABS_X;
  else if (!strcmp(argv[1], "ly")) axis = ABS_Y;
  int val = atoi(argv[2]);
  int ms = atoi(argv[3]);

  int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
  if (fd < 0) { perror("open uinput"); return 1; }

  ioctl(fd, UI_SET_EVBIT, EV_KEY);
  ioctl(fd, UI_SET_KEYBIT, BTN_SOUTH);
  ioctl(fd, UI_SET_KEYBIT, BTN_EAST);
  ioctl(fd, UI_SET_KEYBIT, BTN_NORTH);
  ioctl(fd, UI_SET_KEYBIT, BTN_WEST);
  ioctl(fd, UI_SET_KEYBIT, BTN_START);
  ioctl(fd, UI_SET_KEYBIT, BTN_SELECT);
  ioctl(fd, UI_SET_KEYBIT, BTN_TL);
  ioctl(fd, UI_SET_KEYBIT, BTN_TR);
  ioctl(fd, UI_SET_EVBIT, EV_ABS);
  ioctl(fd, UI_SET_ABSBIT, ABS_X);
  ioctl(fd, UI_SET_ABSBIT, ABS_Y);
  ioctl(fd, UI_SET_ABSBIT, ABS_RX);
  ioctl(fd, UI_SET_ABSBIT, ABS_RY);
  ioctl(fd, UI_SET_ABSBIT, ABS_HAT0X);
  ioctl(fd, UI_SET_ABSBIT, ABS_HAT0Y);

  struct uinput_user_dev uidev;
  memset(&uidev, 0, sizeof(uidev));
  snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "vgpad Controller");
  uidev.id.bustype = BUS_USB;
  uidev.id.vendor = 0x045e;
  uidev.id.product = 0x028e;
  uidev.id.version = 1;
  for (int a = 0; a < ABS_CNT; a++) {
    uidev.absmin[a] = -32767;
    uidev.absmax[a] = 32767;
  }
  write(fd, &uidev, sizeof(uidev));
  ioctl(fd, UI_DEV_CREATE);
  sleep(2); /* deixa o SDL detectar o novo controller (hotplug) */

  /* centro */
  emit(fd, EV_ABS, ABS_X, 0); emit(fd, EV_ABS, ABS_Y, 0);
  emit(fd, EV_ABS, ABS_RX, 0); emit(fd, EV_ABS, ABS_RY, 0);
  syn(fd);
  usleep(200000);
  /* segura o eixo */
  emit(fd, EV_ABS, axis, val);
  syn(fd);
  usleep((useconds_t)ms * 1000);
  /* centraliza */
  emit(fd, EV_ABS, axis, 0);
  syn(fd);
  usleep(300000);

  ioctl(fd, UI_DEV_DESTROY);
  close(fd);
  return 0;
}
