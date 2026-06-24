# 🧱 Texturas: ETC1, ETC2 e o tier da GPU

Texturas dominam a VRAM. A compressão certa depende da GPU do device.

## 1. Qual formato por GPU
* **Mali-450 (Utgard, GLES 2.0):** **ETC1** (sem alpha) é o pão-com-manteiga. Não tem ETC2/ASTC por hardware em ES2.
* **Mali-G31 / RK3326 (GLES 3.x):** **ETC2** disponível (com alpha), mas cuidado com RAM (ver abaixo).
* **X5M (Valhall, ES3 real):** ASTC/ETC2 à vontade.

## 2. Bake offline, não em runtime
Converter textura em runtime engasga a CPU fraca. Faça um **cache offline** (ex.: `etc2cache`, `etc2cache_half`) gerado no PC e empacotado/baked. O loader só faz upload do bloco já comprimido.

## 3. O bug do path "stale" no render target (lição do Bully no R36S)
Sintoma: gameplay 3D **branco chapado** (HUD ok). Causa: o render target da cena herdava o `cur_tex_path` da última `.tex` aberta (setado ao abrir textura, nunca limpo) → casava no cache → virava ETC2 **dentro do FBO** → FBO incompleto → branco.
* **Fix:** flag `path_fresh` — o path só vale pra **1ª textura** após abrir a `.tex`; render target → `pf=0` → sem ETC2 no RT.
* ⚠️ Pista falsa foi o filtro/mipmap: FBO **incompleto** dá **preto**, não branco. Branco = formato inválido no color attachment.

## 4. LUMINANCE → RGBA
A Mali-450 lê `GL_LUMINANCE` como `(L,L,L,L)` (alpha vira brilho). Converta pra `RGBA8888` na CPU antes do upload. (Detalhe em [domando a Mali-450](03-domando-a-mali450.md).)

## 5. Meia-res quando a RAM aperta
Em 1 GB, rebaixar o bake inteiro pra 1/2 (ex.: `BULLY_HALVEBAKE`) cabe na RAM. Mas lembre: **stutter por RAM não é problema de GPU** — se roda liso num device com mais RAM, o muro é memória, não shader.

---
*Resumo: ETC1 no Utgard / ETC2 no G31 / ASTC no Valhall; bake offline; cuidado com path stale no FBO (branco) e com LUMINANCE.*
