/* vgpad: gamepad virtual via uinput p/ testar jogos (gira camera/anda).
 * uso: ./vgpad rx <val -32767..32767> <ms>   (analogico direito X)
 *      ./vgpad ry <val> <ms> | lx | ly
 *      ./vgpad btn <south|east|north|west|start|select|tl|tr> <ms>
 *      ./vgpad hat x|y <-1|1> <ms>
 *      ./vgpad seq <tok> [tok...]   toks: btn:south:300  hat:y:-1:200
 *                                         ax:lx:-32767:500  sleep:1500
 * cria o pad UMA vez, executa e sai (evita hotplug por comando). */
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

static int btn_code(const char *s) {
  if (!strcmp(s, "south")) return BTN_SOUTH;
  if (!strcmp(s, "east")) return BTN_EAST;
  if (!strcmp(s, "north")) return BTN_NORTH;
  if (!strcmp(s, "west")) return BTN_WEST;
  if (!strcmp(s, "start")) return BTN_START;
  if (!strcmp(s, "select")) return BTN_SELECT;
  if (!strcmp(s, "tl")) return BTN_TL;
  if (!strcmp(s, "tr")) return BTN_TR;
  return -1;
}
static int axis_code(const char *s) {
  if (!strcmp(s, "rx")) return ABS_RX;
  if (!strcmp(s, "ry")) return ABS_RY;
  if (!strcmp(s, "lx")) return ABS_X;
  if (!strcmp(s, "ly")) return ABS_Y;
  return -1;
}

static void press_btn(int fd, int code, int ms) {
  emit(fd, EV_KEY, code, 1);
  syn(fd);
  usleep((useconds_t)ms * 1000);
  emit(fd, EV_KEY, code, 0);
  syn(fd);
  usleep(150000);
}
static void hold_axis(int fd, int code, int val, int ms) {
  emit(fd, EV_ABS, code, val);
  syn(fd);
  usleep((useconds_t)ms * 1000);
  emit(fd, EV_ABS, code, 0);
  syn(fd);
  usleep(150000);
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr,
            "uso: %s rx|ry|lx|ly <val> <ms> | btn <nome> <ms> | "
            "hat x|y <val> <ms> | seq <tok...>\n",
            argv[0]);
    return 1;
  }

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

  if (!strcmp(argv[1], "seq")) {
    for (int i = 2; i < argc; i++) {
      char tok[96];
      snprintf(tok, sizeof(tok), "%s", argv[i]);
      char *p1 = strchr(tok, ':');
      if (!p1) continue;
      *p1++ = 0;
      if (!strcmp(tok, "sleep")) {
        usleep((useconds_t)atoi(p1) * 1000);
        continue;
      }
      char *p2 = strchr(p1, ':');
      if (!p2) continue;
      *p2++ = 0;
      if (!strcmp(tok, "btn")) {
        int c = btn_code(p1);
        if (c >= 0) { fprintf(stderr, "[vgpad] btn %s %sms\n", p1, p2); press_btn(fd, c, atoi(p2)); }
      } else if (!strcmp(tok, "hat")) {
        char *p3 = strchr(p2, ':');
        if (!p3) continue;
        *p3++ = 0;
        int c = strcmp(p1, "y") ? ABS_HAT0X : ABS_HAT0Y;
        fprintf(stderr, "[vgpad] hat %s %s %sms\n", p1, p2, p3);
        hold_axis(fd, c, atoi(p2), atoi(p3));
      } else if (!strcmp(tok, "ax")) {
        char *p3 = strchr(p2, ':');
        if (!p3) continue;
        *p3++ = 0;
        int c = axis_code(p1);
        if (c >= 0) { fprintf(stderr, "[vgpad] ax %s %s %sms\n", p1, p2, p3); hold_axis(fd, c, atoi(p2), atoi(p3)); }
      }
    }
  } else if (!strcmp(argv[1], "btn")) {
    if (argc < 4) { fprintf(stderr, "uso: btn <nome> <ms>\n"); return 1; }
    int c = btn_code(argv[2]);
    if (c < 0) { fprintf(stderr, "botao desconhecido: %s\n", argv[2]); return 1; }
    press_btn(fd, c, atoi(argv[3]));
  } else if (!strcmp(argv[1], "hat")) {
    if (argc < 5) { fprintf(stderr, "uso: hat x|y <val> <ms>\n"); return 1; }
    hold_axis(fd, strcmp(argv[2], "y") ? ABS_HAT0X : ABS_HAT0Y, atoi(argv[3]),
              atoi(argv[4]));
  } else {
    if (argc < 4) { fprintf(stderr, "uso: <eixo> <val> <ms>\n"); return 1; }
    int c = axis_code(argv[1]);
    if (c < 0) { fprintf(stderr, "eixo desconhecido: %s\n", argv[1]); return 1; }
    hold_axis(fd, c, atoi(argv[2]), atoi(argv[3]));
  }

  usleep(300000);
  ioctl(fd, UI_DEV_DESTROY);
  close(fd);
  return 0;
}
