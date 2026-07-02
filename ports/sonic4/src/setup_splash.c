/* setup_splash.c -- tela de SETUP ("bake") do Sonic 4 EP2, SDL2 renderer puro,
 * igual ao Bully v11 (fonte 5x7 desenhada em retangulos, barra de progresso,
 * assinatura NEXTOS no canto inferior direito). Standalone: nao carrega libfox.
 * Gate: SONIC_SETUPSPLASH=1. Protocolo: le "estado feito total\nMENSAGEM" de
 * SONIC_SETUP_FILE (default /tmp/sonic_setup.txt); para quando SONIC_SETUP_STOP
 * (default /tmp/sonic_setup_stop) existir. estado 2 = erro (barra vermelha). */
#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int sp_file_exists(const char *path) {
  struct stat st;
  return path && stat(path, &st) == 0;
}

static const char **glyph5x7(int c) {
  static const char *sp[] = {"00000", "00000", "00000", "00000", "00000", "00000", "00000"};
  static const char *q[] = {"01110", "10001", "00001", "00010", "00100", "00000", "00100"};
  static const char *dot[] = {"00000", "00000", "00000", "00000", "00000", "01100", "01100"};
  static const char *dash[] = {"00000", "00000", "00000", "11110", "00000", "00000", "00000"};
  static const char *slash[] = {"00001", "00010", "00010", "00100", "01000", "01000", "10000"};
  static const char *colon[] = {"00000", "01100", "01100", "00000", "01100", "01100", "00000"};
  static const char *pct[] = {"11001", "11010", "00100", "01000", "10110", "00110", "00000"};
  static const char *n0[] = {"01110", "10001", "10011", "10101", "11001", "10001", "01110"};
  static const char *n1[] = {"00100", "01100", "00100", "00100", "00100", "00100", "01110"};
  static const char *n2[] = {"01110", "10001", "00001", "00010", "00100", "01000", "11111"};
  static const char *n3[] = {"11110", "00001", "00001", "01110", "00001", "00001", "11110"};
  static const char *n4[] = {"00010", "00110", "01010", "10010", "11111", "00010", "00010"};
  static const char *n5[] = {"11111", "10000", "10000", "11110", "00001", "00001", "11110"};
  static const char *n6[] = {"00110", "01000", "10000", "11110", "10001", "10001", "01110"};
  static const char *n7[] = {"11111", "00001", "00010", "00100", "01000", "01000", "01000"};
  static const char *n8[] = {"01110", "10001", "10001", "01110", "10001", "10001", "01110"};
  static const char *n9[] = {"01110", "10001", "10001", "01111", "00001", "00010", "01100"};
  static const char *A[] = {"01110", "10001", "10001", "11111", "10001", "10001", "10001"};
  static const char *B[] = {"11110", "10001", "10001", "11110", "10001", "10001", "11110"};
  static const char *C[] = {"01110", "10001", "10000", "10000", "10000", "10001", "01110"};
  static const char *D[] = {"11110", "10001", "10001", "10001", "10001", "10001", "11110"};
  static const char *E[] = {"11111", "10000", "10000", "11110", "10000", "10000", "11111"};
  static const char *F[] = {"11111", "10000", "10000", "11110", "10000", "10000", "10000"};
  static const char *G[] = {"01110", "10001", "10000", "10111", "10001", "10001", "01110"};
  static const char *H[] = {"10001", "10001", "10001", "11111", "10001", "10001", "10001"};
  static const char *I[] = {"01110", "00100", "00100", "00100", "00100", "00100", "01110"};
  static const char *J[] = {"00001", "00001", "00001", "00001", "10001", "10001", "01110"};
  static const char *K[] = {"10001", "10010", "10100", "11000", "10100", "10010", "10001"};
  static const char *L[] = {"10000", "10000", "10000", "10000", "10000", "10000", "11111"};
  static const char *M[] = {"10001", "11011", "10101", "10101", "10001", "10001", "10001"};
  static const char *N[] = {"10001", "11001", "10101", "10011", "10001", "10001", "10001"};
  static const char *O[] = {"01110", "10001", "10001", "10001", "10001", "10001", "01110"};
  static const char *P[] = {"11110", "10001", "10001", "11110", "10000", "10000", "10000"};
  static const char *Q[] = {"01110", "10001", "10001", "10001", "10101", "10010", "01101"};
  static const char *R[] = {"11110", "10001", "10001", "11110", "10100", "10010", "10001"};
  static const char *S[] = {"01111", "10000", "10000", "01110", "00001", "00001", "11110"};
  static const char *T[] = {"11111", "00100", "00100", "00100", "00100", "00100", "00100"};
  static const char *U[] = {"10001", "10001", "10001", "10001", "10001", "10001", "01110"};
  static const char *V[] = {"10001", "10001", "10001", "10001", "10001", "01010", "00100"};
  static const char *W[] = {"10001", "10001", "10001", "10101", "10101", "10101", "01010"};
  static const char *X[] = {"10001", "10001", "01010", "00100", "01010", "10001", "10001"};
  static const char *Y[] = {"10001", "10001", "01010", "00100", "00100", "00100", "00100"};
  static const char *Z[] = {"11111", "00001", "00010", "00100", "01000", "10000", "11111"};
  if (c >= 'a' && c <= 'z')
    c = toupper(c);
  switch (c) {
  case '0': return n0; case '1': return n1; case '2': return n2; case '3': return n3; case '4': return n4;
  case '5': return n5; case '6': return n6; case '7': return n7; case '8': return n8; case '9': return n9;
  case 'A': return A; case 'B': return B; case 'C': return C; case 'D': return D; case 'E': return E;
  case 'F': return F; case 'G': return G; case 'H': return H; case 'I': return I; case 'J': return J;
  case 'K': return K; case 'L': return L; case 'M': return M; case 'N': return N; case 'O': return O;
  case 'P': return P; case 'Q': return Q; case 'R': return R; case 'S': return S; case 'T': return T;
  case 'U': return U; case 'V': return V; case 'W': return W; case 'X': return X; case 'Y': return Y; case 'Z': return Z;
  case '.': return dot; case '-': return dash; case '/': return slash; case ':': return colon; case '%': return pct;
  case ' ': return sp; default: return q;
  }
}

static void draw_text(SDL_Renderer *r, int x, int y, int scale, const char *s) {
  SDL_Rect px = {0, 0, scale, scale};
  for (const unsigned char *p = (const unsigned char *)s; p && *p; p++) {
    const char **g = glyph5x7(*p);
    for (int yy = 0; yy < 7; yy++) {
      for (int xx = 0; xx < 5; xx++) {
        if (g[yy][xx] == '1') {
          px.x = x + xx * scale;
          px.y = y + yy * scale;
          SDL_RenderFillRect(r, &px);
        }
      }
    }
    x += 6 * scale;
  }
}

static int read_setup_state(const char *path, int *state, int *done, int *total,
                            char *msg, size_t msg_sz) {
  FILE *f = fopen(path, "r");
  if (!f)
    return 0;
  char line[256];
  if (fgets(line, sizeof(line), f))
    sscanf(line, "%d %d %d", state, done, total);
  if (msg && msg_sz) {
    if (fgets(msg, (int)msg_sz, f)) {
      msg[msg_sz - 1] = 0;
      msg[strcspn(msg, "\r\n")] = 0;
    }
  }
  fclose(f);
  return 1;
}

int sonic_run_setup_splash(void) {
  const char *setup = getenv("SONIC_SETUP_FILE");
  const char *stop = getenv("SONIC_SETUP_STOP");
  if (!setup || !*setup)
    setup = "/tmp/sonic_setup.txt";
  if (!stop || !*stop)
    stop = "/tmp/sonic_setup_stop";

  /* RETRY: no NextOS novo a ES acabou de ser parada e o DRM pode demorar a ser
     liberado -> a 1a criacao de janela kmsdrm falha. Tentar por ate ~15s antes
     de desistir (a extracao continua por baixo de qualquer forma). */
  SDL_Window *w = NULL;
  SDL_Renderer *r = NULL;
  for (int try = 0; try < 30 && !r; try++) {
    if (sp_file_exists(stop))
      return 0;                                  /* extracao ja acabou */
    /* Se um SDL_VIDEODRIVER herdado (ex.: kmsdrm do PortMaster no ArchR, com o
       sway segurando o DRM) nao abre em ~3s, solta o driver e deixa o SDL
       escolher sozinho (wayland/x11/fbdev — o que a sessao oferecer). */
    if (try == 6 && getenv("SDL_VIDEODRIVER")) {
      fprintf(stderr, "[setup] driver herdado '%s' nao abre -> deixando o SDL escolher\n",
              getenv("SDL_VIDEODRIVER"));
      unsetenv("SDL_VIDEODRIVER");
    }
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
      fprintf(stderr, "[setup] SDL_Init falhou (try %d): %s\n", try, SDL_GetError());
      SDL_Quit();
      usleep(500 * 1000);
      continue;
    }
    w = SDL_CreateWindow("Sonic 4 EP2 setup", SDL_WINDOWPOS_UNDEFINED,
                         SDL_WINDOWPOS_UNDEFINED, 640, 480,
                         SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (!w) {
      fprintf(stderr, "[setup] janela falhou (try %d): %s\n", try, SDL_GetError());
      SDL_Quit();
      usleep(500 * 1000);
      continue;
    }
    r = SDL_CreateRenderer(w, -1, SDL_RENDERER_ACCELERATED);
    if (!r)
      r = SDL_CreateRenderer(w, -1, SDL_RENDERER_SOFTWARE);
    if (!r) {
      fprintf(stderr, "[setup] renderer falhou (try %d): %s\n", try, SDL_GetError());
      SDL_DestroyWindow(w);
      w = NULL;
      SDL_Quit();
      usleep(500 * 1000);
    }
  }
  if (!r) {
    fprintf(stderr, "[setup] sem video apos retries -> desisto (extracao segue)\n");
    return 1;
  }
  {
    SDL_RendererInfo ri;
    int ww = 0, wh = 0;
    SDL_GetRendererOutputSize(r, &ww, &wh);
    fprintf(stderr, "[setup] BAKE OK: videodriver=%s renderer=%s output=%dx%d\n",
            SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?",
            SDL_GetRendererInfo(r, &ri) == 0 ? ri.name : "?", ww, wh);
  }

  while (!sp_file_exists(stop)) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      if (ev.type == SDL_QUIT)
        goto out;
    }

    int state = 1, done = 0, total = 0;
    char msg[180] = "EXTRAINDO DADOS - AGUARDE";
    read_setup_state(setup, &state, &done, &total, msg, sizeof(msg));
    if (total <= 0)
      total = 1;
    if (done < 0)
      done = 0;
    if (done > total)
      done = total;

    int ww = 640, wh = 480;
    SDL_GetRendererOutputSize(r, &ww, &wh);
    SDL_SetRenderDrawColor(r, 4, 7, 12, 255);
    SDL_RenderClear(r);
    SDL_SetRenderDrawColor(r, 236, 238, 224, 255);
    draw_text(r, ww / 2 - 14 * 6 * 4 / 2, wh / 2 - 95, 4, "SONIC 4 EP2 V5");
    draw_text(r, ww / 2 - (int)strlen(msg) * 6 * 2 / 2, wh / 2 - 42, 2, msg);

    SDL_Rect bar = {ww / 2 - 180, wh / 2 + 8, 360, 22};
    SDL_SetRenderDrawColor(r, 236, 238, 224, 255);
    SDL_RenderDrawRect(r, &bar);
    SDL_Rect fill = {bar.x + 3, bar.y + 3, (bar.w - 6) * done / total, bar.h - 6};
    SDL_SetRenderDrawColor(r, state == 2 ? 180 : 86, state == 2 ? 68 : 174,
                           state == 2 ? 68 : 118, 255);
    SDL_RenderFillRect(r, &fill);

    char prog[64];
    snprintf(prog, sizeof(prog), "%d / %d MB", done, total);
    SDL_SetRenderDrawColor(r, 236, 238, 224, 255);
    draw_text(r, ww / 2 - (int)strlen(prog) * 6 * 2 / 2, wh / 2 + 48, 2, prog);

    /* assinatura no canto inferior direito (igual Bully v11) */
    SDL_SetRenderDrawColor(r, 150, 160, 145, 255);
    draw_text(r, ww - 6 * 6 * 2 - 12, wh - 7 * 2 - 10, 2, "NEXTOS");

    SDL_RenderPresent(r);
    SDL_Delay(100);
  }

out:
  SDL_DestroyRenderer(r);
  SDL_DestroyWindow(w);
  SDL_Quit();
  return 0;
}
