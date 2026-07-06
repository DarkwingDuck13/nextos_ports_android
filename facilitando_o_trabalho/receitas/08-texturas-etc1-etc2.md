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

## 6. ETC1 **com alpha**: a dupla camada (Mali-450)
ETC1 não tem canal alpha — mas nada impede usar **duas texturas ETC1**: uma
com o RGB e uma **gêmea** com o alpha gravado como cinza `(A,A,A)`. O fragment
shader amostra as duas e monta o RGBA:

```glsl
uniform sampler2D u_atex;   // gemea (alpha como cinza)
uniform float u_dual;       // 1.0 = essa textura tem gemea
vec4 ss_tex(sampler2D t, vec2 uv) {
    vec4 c = texture2D(t, uv);
    if (u_dual > 0.5) c.a = texture2D(u_atex, uv).g;
    return c;
}
```

* **Custo:** 4bpp + 4bpp = **8bpp** com alpha (RGBA8888 = 32bpp → **4x menos
  RAM**; RGBA4444 = 16bpp → 2x menos). Opaco fica em 4bpp puro (8x).
  No Mali-450 textura mora na RAM do sistema — isso é corte de RAM real.
* **Duas texturas > altura dupla.** A alternativa clássica (uma ETC1 de altura
  2x, RGB em cima / alpha embaixo) exige remapear UV no shader e sofre
  **bleeding do filtro bilinear na emenda** das metades. Com textura gêmea os
  UVs ficam idênticos e não há emenda. O preço é 1 id GL a mais e a limpeza
  casada (deletou a principal → deleta a gêmea).
* **Unidade de textura:** ligue a gêmea numa unidade ALTA (`MAX_UNITS-1`, 7 no
  Mali-450) pra não brigar com as unidades que a engine usa (0..2).
* **Premultiplied:** se a engine composita premultiplied (Ren'Py gl2, quase
  todo 2D moderno), **premultiplique o RGB antes de codificar**. O ETC1 até
  comprime melhor (bordas transparentes → RGB escuro suave) e o bilinear fica
  correto.
* **Texto NÃO:** alpha de glifo em blocos 4x4 borra a borda da fonte. Deixe
  texto/HUD fino em 4444/8888.

## 7. Runtime encode: quando compensa (e o encoder de ~250 linhas)
A regra da seção 2 ("bake offline") vale pra jogo 3D com streaming. Mas em
jogo 2D/VN que carrega imagem **esporadicamente** (troca de cena/expressão),
codificar ETC1 **em runtime no upload** funciona bem e não muda NENHUM asset:

* Encoder heurístico próprio (clean-room, sem GPL): base = média do subbloco
  (diferencial 555+333 quando cabe, senão individual 444), busca completa
  tabela(8)×modificador(4) por pixel com poda. ~250 linhas de C, sem NEON já
  segura VN (fundo 800x450 ≈ dezenas de ms, 1x por cena).
* Layout do bloco (64 bits, big-endian): bytes 0-2 = cores-base, byte 3 =
  `tabela1[7:5] tabela2[4:2] diff[1] flip[0]`, bytes 4-7 = índices em DOIS
  planos de 16 bits (MSB antes de LSB), **texel i = coluna*4+linha**
  (column-major!). Índice `(msb,lsb)`: `00`=+pequeno `01`=+grande
  `10`=−pequeno `11`=−grande.
* **Valide no PC antes do device:** escreva um DECODER independente direto da
  spec e faça round-trip com PSNR (sólido >38dB, gradiente >30dB, e cheque
  que cinza decodifica com R=G=B). Pega bug de bit-packing sem queimar ciclo
  de deploy. (Teste pronto em `ports/summertimesaga/` + scratch `etc1_test.c`.)
* Upload: `glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_ETC1_RGB8_OES=0x8D64,
  w, h, 0, ceil(w/4)*ceil(h/4)*8, dados)`. Dimensão NÃO precisa ser múltipla
  de 4 (borda replicada no encode). **TexSubImage em ETC1 é inválido** — se a
  engine atualiza retângulo, essa textura não pode ser ETC1.

## 8. Caso real: Ren'Py/gl2 (Summertime Saga) — a pegadinha do premultiply
Wrapper 16-bit no `glTexImage2D` e a RAM não caía? A textura residente não
nascia ali: o gl2 sobe um upload **temporário**, premultiplica na GPU (passe
FTL num FBO) e a textura final vem de **`glCopyTexImage2D` full-size
RGBA8888** — fora do alcance de qualquer conversão no upload. Lição: **trace
QUEM define o level 0 residente** (TexImage com dados? CopyTexImage? NULL +
SubImage?) antes de otimizar formato.

A saída (tudo sem rebuild do engine, arquivos `.py` + wrapper C):
1. O Ren'Py já tem um caminho direto `load_gltexture_premultiplied()` (upload
   único, interceptável). Monkeypatch em `Texture.load` (classe Python!) troca
   imagens do jogo pra ele; texto (já premultiplied) e vídeo ficam no caminho
   antigo.
2. O `.py` avisa o binário via `ctypes.CDLL(None).ss_tex_hint(1)` (símbolo
   exportado com `-rdynamic`) na hora exata do upload → o wrapper C
   premultiplica na conversão (de graça), aplica downscale e codifica ETC1
   dupla camada.
3. Shader: os GLSL do gl2 são montados em Python (`_shaders.rpym`), mas o
   remendo foi no `glShaderSource` do wrapper C — invisível pra engine (o
   Ren'Py parseia o PRÓPRIO fonte pra achar uniforms, nunca vê os nossos).
   Troca textual `texture2D(texN,` → `ss_texN(texN,` + prelúdio com overloads
   de 2 e 3 argumentos (o gl2 usa `texture2D(tex, uv, bias)`).
4. No draw: wrapper rastreia `glUniform1i`/`glActiveTexture`/`glBindTexture`
   pra saber qual textura está em `tex0..tex2`, e seta `u_ss_dualN` + liga a
   gêmea na unidade alta. Validar os shaders remendados com
   `glslangValidator` no PC.

Resultado teórico por cena: fundo 1920x1080 RGBA8888 (8.3MB) → 800x450 ETC1
(0.18MB, **46x**); camada de personagem com alpha 600x1000 (2.4MB) → dupla
camada (0.6MB, **4x**).

---
*Resumo: ETC1 no Utgard / ETC2 no G31 / ASTC no Valhall; bake offline em 3D,
runtime encode serve pra 2D/VN; alpha no Utgard = dupla camada ETC1 (RGB +
gêmea cinza) com shader remendado; premultiplique antes de codificar; texto
fica fora; e sempre confira QUEM define a textura residente.*
