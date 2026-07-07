# 🎯 Candidatos a portar — backlog

Jogos ainda **não** portados pelo NextOS, com engine, port-irmão de referência
(Vita/Switch, quando existe) e dificuldade estimada pro **Mali-450 (Utgard)**.
Regra de ouro: **ES2 nativo < ES3 (shimável) < UE4 ES3.1 (muro) < Vulkan (muro absoluto no Utgard)**.
Tudo **BYO-data** — o repo nunca traz o jogo.

> Referências da cena so-loader: **TheFloW** (Vita), **Rinnegatamante** (Vita, `yoyoloader`),
> **v-atamanenko** (Vita, `FalsoJNI`), **NaGaa95** (Switch), **mtojek** (Linux ARM64).

## 🟢 Prioridade alta — mesma engine que já dominamos

| Jogo | Engine | Referência irmã | Por que é viável |
|---|---|---|---|
| **GTA: San Andreas** | RenderWare (ES3, shimável) | TheFloW `gtasa_vita`, NaGaa95 `gtasa_nx` | Mesma família de Vice City + LCS (já rodam). Shim ES3 dominado. |
| **Max Payne Mobile** | RenderWare (Rockstar) | NaGaa95 `max_nx` | Idem SA — engine irmã dos GTA. |
| **GTA: Chinatown Wars** | RenderWare | TheFloW `gtactw_vita`, NaGaa95 `gtactw_nx` | Família GTA. |
| **LEGO: A Força Desperta** | TT/Fusion | NaGaa95 `lswtfa_nx` | Mesma engine do nosso LEGO Batman 3 + LEGO SW (ref). |
| **LEGO Batman 2: DC Super Heroes** | TT/Fusion | NaGaa95 `lbdcsh_nx` | Idem. |
| **LEGO Ninjago: Shadow of Ronin** | TT/Fusion | NaGaa95 `lnsor_nx` | Idem. |
| **FF III / IV (3D), FF IV: The After Years, FF Dimensions 1 e 2** | Square mobile (nativa/Unity) | Rinne `ff4_vita`/`ff4a_vita`, NaGaa `ff3_3d/ff4_3d/ffd/ffd2` | Bagagem de FF7/FF9. FF4 = GLES1 fixed-function. |
| **Catálogo GameMaker/YYC** (dezenas) | GameMaker | Rinne `yoyoloader` + [compat list](https://github.com/Rinnegatamante/YoYo-Loader-Vita-Compatibility) | Com o loader genérico (receita 15): largar o APK. |

> **Já concluídos (jogáveis, ver README):** Battlefield: Bad Company 2, Castle of Illusion.

## 🟡 Médio / grande (viável, mais trabalho)

| Jogo | Engine | Referência irmã | Nota |
|---|---|---|---|
| **Half-Life 2** | Source | NaGaa95 `hl2_nx` | Source roda em ARM; pesado. |
| **Counter-Strike: Source** | Source | NaGaa95 `css_nx` | Idem. |
| **Team Fortress 2** | Source | NaGaa95 `tf2_nx` | Idem. |
| **Soul Calibur** | Namco (nativa) | Rinne `soulcalibur_vita` | 3D fighting. |
| **The Conduit HD** | nativa | TheFloW/Rinne `conduit_vita` | FPS. |
| **Fahrenheit / Indigo Prophecy** | nativa | TheFloW `fahrenheit_vita` | Aventura. |
| **Professor Layton 1 / 2 / 3** | Level-5 | NaGaa95 `layton*_nx` | Puzzle/aventura. |
| **After Burner Climax** | Sega | NaGaa95 `abc_nx` | Arcade. |
| **Swordigo** | nativa 2.5D | NaGaa95 `swordigo_nx` | Leve. |

## 🟢 Menores / indie (bons pra validar o loader genérico)

World of Goo (`goo`) · Geometry Dash (`gdash`) · Jet Car Stunts · Desert Golfing ·
32 Secs · série Anomaly (Warzone Earth / 2 / Korea / Defenders) · Pickleball · Funky Smugglers.

## ⚠️ Caminho diferente (recomp de engine open-source, NÃO so-loader)

VCMI (Heroes of Might & Magic III) · FreeSpace 2 · OpenMoHAA (Medal of Honor).
São reimplementações compiláveis pra ARM — categoria do Dusklight, não so-loader de APK.

## 🔴 Muro no Utgard (só device com Vulkan/ES3, ex. X5M)

Red Dead Redemption (Vulkan puro) · UE4 AAA (Afterimage, Little Nightmares, Bloodstained) ·
Minecraft Bedrock moderno (RenderDragon ES3/Vulkan).

---

**Ordem sugerida de ataque:** (1) **GTA San Andreas** e **Max Payne** (RenderWare = VC/LCS,
risco baixo) → (2) **família LEGO** (engine do LEGO Batman 3) → (3) **FF III/IV/Dimensions**
(bagagem Square) → (4) **loader genérico GameMaker** (receita 15) pra colher dezenas de uma vez
→ (5) Source engine (HL2/CS:S/TF2) quando quiser um desafio grande.
