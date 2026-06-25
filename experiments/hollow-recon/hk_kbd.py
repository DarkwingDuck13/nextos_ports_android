#!/usr/bin/env python3
# hk_kbd.py -- TECLADO virtual via uinput. A HK (Rewired) abre os devices de TECLADO
# (event0 gpio_keypad, etc.) e NAO o gamepad (provado por fd-probe). Entao injetamos
# TECLAS de navegacao (setas + Enter/Z/X) p/ avancar os menus. Uso: hk_kbd.py [delay] [dur]
import sys, time, os
from evdev import UInput, ecodes as e

DELAY = float(sys.argv[1]) if len(sys.argv) > 1 else 25.0
DUR   = float(sys.argv[2]) if len(sys.argv) > 2 else 90.0
MODE  = os.environ.get('HK_KBDMODE', 'nav')   # 'down' = so desce p/ ver cursor; 'nav' = navega+confirma

KEYS = [
    e.KEY_UP, e.KEY_DOWN, e.KEY_LEFT, e.KEY_RIGHT,
    e.KEY_ENTER, e.KEY_KPENTER, e.KEY_SPACE, e.KEY_ESC,
    e.KEY_Z, e.KEY_X, e.KEY_C, e.KEY_RETURN if hasattr(e, 'KEY_RETURN') else e.KEY_ENTER,
    e.KEY_W, e.KEY_A, e.KEY_S, e.KEY_D, e.KEY_J, e.KEY_K,
]
cap = { e.EV_KEY: sorted(set(KEYS)) }
ui = UInput(cap, name='HK Virtual Keyboard', vendor=0x1209, product=0x0001, version=0x1)
print('[hk_kbd] teclado virtual criado: %s' % ui.device.path, flush=True)

def tap(code, hold=0.07):
    ui.write(e.EV_KEY, code, 1); ui.syn(); time.sleep(hold)
    ui.write(e.EV_KEY, code, 0); ui.syn(); time.sleep(0.13)

print('[hk_kbd] esperando %.0fs p/ HK escanear...' % DELAY, flush=True)
time.sleep(DELAY)
t0 = time.time(); n = 0
print('[hk_kbd] injetando teclas modo=%s' % MODE, flush=True)
while time.time() - t0 < DUR:
    n += 1
    if MODE == 'down':
        tap(e.KEY_DOWN)                         # so desce -> ver cursor mover
    else:
        # navega + tenta TODAS as teclas de confirmar comuns da HK
        tap(e.KEY_DOWN); tap(e.KEY_UP)
        for k in (e.KEY_ENTER, e.KEY_Z, e.KEY_SPACE, e.KEY_X, e.KEY_C):
            tap(k); time.sleep(0.15)
    if n % 4 == 0:
        print('[hk_kbd] ciclo %d' % n, flush=True)
    time.sleep(0.6)
ui.close()
print('[hk_kbd] fim', flush=True)
