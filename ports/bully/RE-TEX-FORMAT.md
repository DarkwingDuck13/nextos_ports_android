# Bully `.tex` — formato de textura (RE 2026-06-19)

RE feita pra viabilizar **conversão ETC1 offline** (M0 do plano `snappy-purring-trinket`).
Fonte: data zips reais em `experiments/bully-pc/gamefiles/assets/` + strings de `libGame.so`.

## Container (já parseado em `src/asset_archive.c`)
- `data_0..4.zip` + `.idx` (`u32 count`; por entrada `u32 off, u32 size, u16 namelen, name`).
  O `off` aponta pro DADO CRU dentro do zip (lido como slice, sem deflate).
- IMG packs RenderWare (`act/scripts/dat/cuts/stream/objects`): blocos 2048B, dir 32B/entrada.
- Texturas = arquivos **`.tex`** com caminho lógico tipo `bully/<nome>.tex`.
  Sufixos: `_d` (diffuse), `_n` (normal), `_s` (specular).
- **O nome chega no runtime via `asset_open(const char *path)`** (`asset_archive.c:530`).
  Contagem (.idx): data_1=105, data_2=527, data_3=1790, data_4=11642 `.tex` (~14k no total).

## Layout do `.tex`
```
[bin header pequeno: u32le ... ; [5] = offset onde o descritor termina+1]
[descritor TEXTO entre { }]  (chaves balanceadas; tem samplerstate{} e df_params{} aninhados)
   {width=W,height=H,mode=tm_standard|tm_nopvr,nomips=false,compressondisk=false,
    addressmode...,samplerstate={...},df_params={...},pp_params={...},
    importfilepath="D:\Bully\360\...\Nome.png"}
[sub-header bin: u32le = [fmt_enum, W, W, levels, datasize, 0xffffffff, hash...]]
   fmt_enum: 9 = tm_standard ;  5 = tm_nopvr
   levels   = log2(maxdim)+1  (cadeia de mip COMPLETA é prevista; ver nota)
[pixel data]
```

## Formato de pixel (provado por tamanho de payload)
Enums do engine (`libGame.so`): `TF_RGB_565 / TF_RGBA_4444 / TF_RGBA_8888 / TF_DXT1/3/5 /
TF_PVRTC4_RGBA`; modos `TM_Standard / TM_Raw16 / TM_Raw32 / TM_SingleChannel / TM_NoPVR / TM_NoATC /
TM_DistanceField`. Há `DecodeDXTColorBlock` e exts `dxt1`/`pvrtc` — mas **Mali-450 (Utgard) não tem
DXT nem PVRTC** → o engine **decodifica na CPU pra 16-bit e sobe via glTexImage2D** (type 0x8033/0x8363,
`bpp_of` em `imports.c:490`). Por isso o boot só mostra uploads RGBA/16-bit.

Amostras (payload = sz − fim_do_descritor):
| textura | desc WxH | payload | payload/(W·H) | conclusão |
|---|---|---|---|---|
| `justin..._d` (data_2) | 1024×512 | 1.048.688 | **2,00** | **16-bit (565/4444), 1 nível** (1024·512·2=1.048.576 + ~112 sub-hdr) |
| `english_background` (data_1) | 2048×1024 | 4.194.416 | **2,00** | 16-bit, 1 nível |
| `orderly02_new_n` (data_3) | 512×512 | 349.632 | 1,33 | **8-bit single-channel + mips** (262144·1,333≈349525) |
| `radar00` (data_4, tm_nopvr) | 8×8 | 76 | — | UI minúscula |

**Conclusão:** os "hogs" de memória são os **diffuse `_d` 16-bit single-level** → transcodam pra
ETC1 (RGB, 0,5 bpp) = **4× menores** → resolve o limite de textura-MMU do Utgard (causa real do
wedge na escola) SEM `TEX_HALF`. Os `_n/_s` são pequenos/single-channel (já eram pulados pelo
`TEX_LIGHT`) → deixar como estão.

## Arquitetura escolhida (offline, zero conversão em runtime, baixo risco)
1. **Baker offline (`bullytexbake`, no progressor):** varre os `.tex`; para os 16-bit opacos
   (`_d`, payload≈W·H·2, sem alpha relevante) lê o nível-0, expande 565/4444→RGB888, **encoda ETC1**
   (reusa `etc1_encode.c`), e grava **sidetable `etc1.cache`** chaveada pelo **caminho do asset**
   (igual modelo dysmantle, mas a chave vem do `asset_open`). Pula `_n/_s`, alpha, e pequenas.
   NÃO modifica os `.tex` (engine intocado = sem risco de rejeição).
2. **Runtime:** `asset_open` seta `g_cur_tex_path`; no `my_glTexImage2D` consulta `etc1.cache[path]`
   — se bate **dimensão+formato** (guarda anti-magenta), sobe ETC1 via `glCompressedTexImage2D`;
   senão passa o RGBA original. **Zero encode em runtime.** Remove `TEX_HALF/LIGHT/budget/despejo`.

## A confirmar no device (.164) — fecha o GO/NO-GO
Instrumentar o hook pra logar `asset_open(path)` + `(level,w,h,type,bytes)` do `glTexImage2D` e
casar nome↔upload (valida 565-vs-4444 por textura e confirma que o nível-0 stored = o que sobe).
Depois: baker em 1 textura `_d`, subir ETC1, e ver renderizar no Mali-450.
