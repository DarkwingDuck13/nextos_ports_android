#!/usr/bin/env python3
# fbcols.py <raw> -- mede a distribuicao HORIZONTAL de conteudo no fb (BGRA 1280x720 dupla-pag).
# Imprime, por pagina: 1a/ultima coluna nao-preta, % de colunas com conteudo, e um histograma
# grosso (32 baldes) -> quantifica o "zoom/offset" (terco esquerdo preto = balde 0-10 vazios).
import sys
from PIL import Image
raw = open(sys.argv[1], 'rb').read()
W, H = 1280, 720
PG = W * H * 4
for pg in range(min(2, len(raw) // PG)):
    img = Image.frombytes('RGBA', (W, H), raw[pg*PG:(pg+1)*PG])
    b, g, r, a = img.split()
    rgb = Image.merge('RGB', (r, g, b))
    px = rgb.load()
    col_has = [0] * W
    for x in range(W):
        for y in range(0, H, 8):  # amostra 1 a cada 8 linhas (rapido)
            if px[x, y] != (0, 0, 0):
                col_has[x] = 1
                break
    nz = [x for x in range(W) if col_has[x]]
    if not nz:
        print(f'pg{pg}: VAZIO (tudo preto)')
        continue
    first, last = nz[0], nz[-1]
    pct = 100.0 * len(nz) / W
    buckets = 32
    hist = ''
    for k in range(buckets):
        a0, a1 = k * W // buckets, (k + 1) * W // buckets
        frac = sum(col_has[a0:a1]) / (a1 - a0)
        hist += '#' if frac > 0.5 else ('.' if frac > 0.05 else ' ')
    print(f'pg{pg}: 1a_col={first} ult_col={last} cols_com_conteudo={pct:.0f}% hist[{hist}]')
