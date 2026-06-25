#!/usr/bin/env python3
# hk_pad.py -- CLONE EXATO do "USB Gamepad" (0810:0001) que a HK espera (perfil
# TInputCustom configurado p/ esse layout: BTN_TRIGGER..BTN_BASE6 + ABS_X/Y/Z/RZ/HAT0).
# Cria o device via uinput e injeta navegacao (brute-force dos botoes de face + DPAD)
# p/ avancar os menus AUTOMATICAMENTE. Uso: python3 hk_pad.py [delay] [duracao]
import sys, time
from evdev import UInput, ecodes as e

DELAY = float(sys.argv[1]) if len(sys.argv) > 1 else 22.0
DUR   = float(sys.argv[2]) if len(sys.argv) > 2 else 90.0

# botoes EXATOS do USB Gamepad original (BTN_TRIGGER=0x120 .. BTN_BASE6=0x12b)
BTNS = list(range(0x120, 0x12c))  # 288..299
cap = {
    e.EV_KEY: BTNS,
    e.EV_ABS: [
        (e.ABS_X,     (128, 0, 255, 0, 15, 0)),
        (e.ABS_Y,     (128, 0, 255, 0, 15, 0)),
        (e.ABS_Z,     (128, 0, 255, 0, 15, 0)),
        (e.ABS_RZ,    (128, 0, 255, 0, 15, 0)),
        (e.ABS_HAT0X, (0, -1, 1, 0, 0, 0)),
        (e.ABS_HAT0Y, (0, -1, 1, 0, 0, 0)),
    ],
}
# identidade IDENTICA ao original -> o mapeamento TInputCustom da HK aplica
ui = UInput(cap, name=' USB Gamepad          ', vendor=0x0810, product=0x0001, version=0x0110)
print('[hk_pad] CLONE USB Gamepad criado: %s' % ui.device.path, flush=True)

def tap(code, hold=0.10):
    ui.write(e.EV_KEY, code, 1); ui.syn(); time.sleep(hold)
    ui.write(e.EV_KEY, code, 0); ui.syn(); time.sleep(0.15)

def hat(axis, val, hold=0.12):
    ui.write(e.EV_ABS, axis, val); ui.syn(); time.sleep(hold)
    ui.write(e.EV_ABS, axis, 0);   ui.syn(); time.sleep(0.18)

print('[hk_pad] esperando %.0fs p/ HK escanear...' % DELAY, flush=True)
time.sleep(DELAY)

# TESTE DE NAVEGACAO: so DOWN (DPAD + stick) p/ ver o cursor DESCER -> prova que o
# input chega na HK. Depois tenta confirmar. HK_PADMODE=down so desce; senao mistura.
import os
MODE = os.environ.get('HK_PADMODE', 'down')
FACE = [0x121, 0x120, 0x122, 0x123]  # THUMB, TRIGGER, THUMB2, TOP
t0 = time.time(); n = 0
print('[hk_pad] navegacao automatica modo=%s' % MODE, flush=True)
while time.time() - t0 < DUR:
    n += 1
    if MODE == 'down':
        # so DESCER: DPAD baixo + stick baixo (sinal claro p/ ver o cursor mover)
        hat(e.ABS_HAT0Y, 1)
        ui.write(e.EV_ABS, e.ABS_Y, 255); ui.syn(); time.sleep(0.12)
        ui.write(e.EV_ABS, e.ABS_Y, 128); ui.syn(); time.sleep(0.2)
    else:
        hat(e.ABS_HAT0Y, 1); hat(e.ABS_HAT0Y, -1)
        for b in FACE:
            tap(b); time.sleep(0.2)
    if n % 3 == 0:
        print('[hk_pad] ciclo %d' % n, flush=True)
    time.sleep(0.7)

ui.close()
print('[hk_pad] fim', flush=True)
