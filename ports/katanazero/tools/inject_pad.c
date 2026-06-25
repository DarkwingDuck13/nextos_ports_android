/* inject_pad.c — injetor uinput de GAMEPAD (layout Xbox 360) p/ testar controle sem humano.
 * Cria um device com vendor/product do Xbox 360 (0x045e/0x028e) que o SDL reconhece como
 * GameController. Reproduz sequencia: "BTN:ms", "HATU/HATD/HATL/HATR:ms", "AXn:val:ms".
 * BTN nomes: A B X Y LB RB SELECT START LS RS. Build: aarch64 toolchain.
 * Uso: ./inject_pad <delay_inicial_ms> CMD CMD ...
 */
#include <linux/uinput.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static int btn_code(const char *n) {
  if (!strcmp(n, "A")) return BTN_SOUTH;
  if (!strcmp(n, "B")) return BTN_EAST;
  if (!strcmp(n, "X")) return BTN_WEST;   /* Xbox: West=X */
  if (!strcmp(n, "Y")) return BTN_NORTH;  /* Xbox: North=Y */
  if (!strcmp(n, "LB")) return BTN_TL;
  if (!strcmp(n, "RB")) return BTN_TR;
  if (!strcmp(n, "SELECT")) return BTN_SELECT;
  if (!strcmp(n, "START")) return BTN_START;
  if (!strcmp(n, "LS")) return BTN_THUMBL;
  if (!strcmp(n, "RS")) return BTN_THUMBR;
  return -1;
}

static void emit(int fd, int type, int code, int val) {
  struct input_event ev; memset(&ev, 0, sizeof ev);
  ev.type = type; ev.code = code; ev.value = val;
  write(fd, &ev, sizeof ev);
}
static void syn(int fd) { emit(fd, EV_SYN, SYN_REPORT, 0); }

int main(int argc, char **argv) {
  int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
  if (fd < 0) { perror("open uinput"); return 1; }
  ioctl(fd, UI_SET_EVBIT, EV_KEY);
  int btns[] = {BTN_SOUTH,BTN_EAST,BTN_NORTH,BTN_WEST,BTN_TL,BTN_TR,BTN_SELECT,BTN_START,
                BTN_MODE,BTN_THUMBL,BTN_THUMBR};
  for (unsigned i = 0; i < sizeof(btns)/sizeof(btns[0]); i++) ioctl(fd, UI_SET_KEYBIT, btns[i]);
  ioctl(fd, UI_SET_EVBIT, EV_ABS);
  int axes[] = {ABS_X,ABS_Y,ABS_RX,ABS_RY,ABS_Z,ABS_RZ,ABS_HAT0X,ABS_HAT0Y};
  for (unsigned i = 0; i < sizeof(axes)/sizeof(axes[0]); i++) ioctl(fd, UI_SET_ABSBIT, axes[i]);

  struct uinput_user_dev uidev; memset(&uidev, 0, sizeof uidev);
  snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "Microsoft X-Box 360 pad");
  uidev.id.bustype = BUS_USB; uidev.id.vendor = 0x045e; uidev.id.product = 0x028e; uidev.id.version = 0x0114;
  for (int i = 0; i < ABS_CNT; i++) {
    if (i==ABS_HAT0X||i==ABS_HAT0Y){ uidev.absmin[i]=-1; uidev.absmax[i]=1; }
    else if (i==ABS_Z||i==ABS_RZ){ uidev.absmin[i]=0; uidev.absmax[i]=255; }
    else { uidev.absmin[i]=-32768; uidev.absmax[i]=32767; }
  }
  write(fd, &uidev, sizeof uidev);
  ioctl(fd, UI_DEV_CREATE);

  int delay = (argc > 1) ? atoi(argv[1]) : 8000;
  fprintf(stderr, "[pad] device criado, esperando %dms...\n", delay);
  usleep(delay * 1000);

  for (int i = 2; i < argc; i++) {
    char buf[64]; strncpy(buf, argv[i], 63); buf[63] = 0;
    char *c1 = strchr(buf, ':'); int ms = 400;
    char *valp = NULL;
    if (c1) { *c1 = 0; char *c2 = strchr(c1+1, ':');
      if (c2) { *c2 = 0; valp = c1+1; ms = atoi(c2+1); } else ms = atoi(c1+1); }
    if (!strncmp(buf, "HAT", 3)) {
      int x=0,y=0; char d=buf[3];
      if (d=='U') y=-1; else if (d=='D') y=1; else if (d=='L') x=-1; else if (d=='R') x=1;
      fprintf(stderr, "[pad] HAT%c\n", d);
      emit(fd,EV_ABS,ABS_HAT0X,x); emit(fd,EV_ABS,ABS_HAT0Y,y); syn(fd); usleep(120000);
      emit(fd,EV_ABS,ABS_HAT0X,0); emit(fd,EV_ABS,ABS_HAT0Y,0); syn(fd);
    } else if (!strcmp(buf, "AIM") && valp) {
      /* right stick para uma direcao (aim): AIM:x,y  ex AIM:30000,0 */
      int ax=0,ay=0; sscanf(valp, "%d,%d", &ax, &ay);
      fprintf(stderr, "[pad] AIM %d,%d\n", ax, ay);
      emit(fd,EV_ABS,ABS_RX,ax); emit(fd,EV_ABS,ABS_RY,ay); syn(fd);
    } else {
      int code = btn_code(buf);
      if (code < 0) { fprintf(stderr, "[pad] cmd? %s\n", buf); continue; }
      fprintf(stderr, "[pad] %s (wait %dms)\n", buf, ms);
      emit(fd,EV_KEY,code,1); syn(fd); usleep(60000);
      emit(fd,EV_KEY,code,0); syn(fd);
    }
    usleep(ms * 1000);
  }
  fprintf(stderr, "[pad] fim, mantendo 3s\n");
  usleep(3000000);
  ioctl(fd, UI_DEV_DESTROY);
  close(fd);
  return 0;
}
