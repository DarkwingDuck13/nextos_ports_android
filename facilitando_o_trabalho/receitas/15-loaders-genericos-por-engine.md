# 🏭 Loaders genéricos por engine — 1 binário, N jogos

> **O que muda:** hoje cada jogo GameMaker (Katana ZERO, Hotline Miami 2) é um
> **port separado**, mesmo os dois sendo `libyoyo.so`. Isso é retrabalho. A ideia
> aqui é ter **um loader por ENGINE** que roda qualquer jogo daquela engine a
> partir de um APK + um `game.cfg` — sem recompilar. Inspirado no `yoyoloader_vita`
> (Rinnegatamante), mas na nossa arquitetura Mali-450/multiarch.

## Quais engines valem a pena

Priorize engines onde vocês já têm **≥2 ports** (a lição já está paga):

| Engine | `.so` | Ports já feitos | Vira loader genérico? |
|---|---|---|---|
| **GameMaker / YYC** | `libyoyo.so` | Katana ZERO, Hotline Miami 2 | ✅ alvo #1 |
| **Cocos2d-x** | `libcocos2d*.so` / `libgame` | Mega Man 1–6, Chrono Trigger | ✅ alvo #2 |
| **Marmalade s3e** | `lib*.so` | CoD BOZ, PES 2012 | 🟡 (s3e varia por app) |
| **Unity IL2CPP** | `libunity.so`+`libil2cpp.so` | Terraria, RE4, MMX, FF9… | ❌ (metadata é por-jogo) |

Unity **não** vira genérico: o `global-metadata.dat` e os offsets são únicos por
build. Mantém por-port. GameMaker e Cocos2d-x sim — a engine é a mesma, só muda o
`game.apk` e alguns flags.

## Layout proposto

```
ports/yoyo/                 # o LOADER genérico (um binário)
  yoyo                      #   binário Mali-450 (BYO: reproduzível do build.sh)
  build.sh
  src/...                   #   loader + nx_jni (tabela) + shims
games/yoyo/<NOME>/          # cada JOGO (BYO-data, fora do repo)
  game.apk                  #   o APK que VOCÊ possui
  game.cfg                  #   config declarativa (vai pro repo? NÃO — é por-jogo do usuário)
```

O loader, no boot, lê `$GAME_DIR/game.cfg`, extrai/mapeia o `.so` de dentro do
`game.apk` e aplica os flags. Trocar de jogo = trocar a pasta. Zero recompilação.

## `game.cfg` — config declarativa (não recompilar)

Formato simples `chave=valor` (parse trivial em C, sem dependência):

```ini
# identidade
package = com.devolver.hotlinemiami2
obb     = 0
so      = libyoyo.so           # qual .so carregar de dentro do APK

# render / device
gles      = 2                  # 1 ou 2
gfxargs   = -force-gfx-direct   # args extras (Unity/engines que leem)
texfmt    = etc1               # etc1 | etc2->etc1 | rgba (política de textura)
scale     = native             # native | 1280x720 | ...

# controle (keymap -> keycode Android, alimenta gptokeyb / input nativo)
btn_a     = 96
btn_b     = 97
dpad      = analog             # analog | hat | keys
hotkey_quit = select+start

# audio
audio     = opensles           # opensles | fmod | none
rate      = 44100
```

**Por que isso importa:** hoje esses valores estão *baked* no binário de cada port
(veja os `BULLY_TEX_*`, `CVGOS_*`, `CUP_GFXARGS`). Externalizar num `game.cfg` deixa
ajustar sem rebuild — e é a base pra uma **lista de compatibilidade** por jogo.

## Passos pra transformar 2 ports num loader genérico

1. **Isolar o que é da engine** (comum aos dois) do que é do jogo (paths, keymap,
   texfmt). O comum vira o loader; o resto vira `game.cfg`.
2. **Ler o `.so` de dentro do APK em runtime** (o loader já sabe abrir zip — o
   `new-port.sh` faz na hora do bootstrap; mova essa lógica pro runtime).
3. **JNI por tabela** ([receita 14](14-jni-por-tabela.md)): a tabela da engine é
   quase toda comum; diferenças por jogo viram entradas condicionadas ao `package`.
4. **Aplicar `game.cfg`** nos pontos que hoje são env/`#define`.
5. **Lista de compatibilidade:** um `COMPAT.md` no loader (jogo → estado → cfg
   recomendado). Igual ao que o yoyoloader faz.

## Ganho esperado

- GameMaker: de "1 port por jogo" → "largou o APK, rodou". Dezenas de jogos YYC.
- Cocos2d-x: idem pra a família (os 6 Mega Man já provam o molde).
- Menos binários no repo, mais jogos cobertos, e config ajustável sem toolchain.
