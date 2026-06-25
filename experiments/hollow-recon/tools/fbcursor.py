#!/usr/bin/env python3
# fbcursor.py <raw...> -- acha a LINHA (y) media do cursor roxo (~132,129,255) em cada
# fb (BGRA 1280x720 dupla-pag). Se o y muda entre capturas, o input ESTA movendo o cursor.
import sys
from PIL import Image
W, H = 1280, 720
PG = W * H * 4
for path in sys.argv[1:]:
    raw = open(path, 'rb').read()
    img = Image.frombytes('RGBA', (W, H), raw[:PG])
    b, g, r, a = img.split()
    rgb = Image.merge('RGB', (r, g, b))
    px = rgb.load()
    ys, xs, nb = [], [], 0
    for y in range(0, H, 2):
        for x in range(0, W, 2):
            R, G, B = px[x, y]
            if B > 180 and 90 < R < 180 and 90 < G < 180 and B - R > 50:  # roxo/lavanda do cursor
                ys.append(y); xs.append(x)
            if (R, G, B) != (0, 0, 0):
                nb += 1
    if ys:
        ys.sort()
        print('%s: cursor_roxo y_med=%d y_min=%d y_max=%d n=%d | px_nao_pretos=%d' %
              (path.split('/')[-1], sum(ys)//len(ys), ys[0], ys[-1], len(ys), nb))
    else:
        print('%s: SEM cursor roxo | px_nao_pretos=%d' % (path.split('/')[-1], nb))
