# Cuphead Mali-450 — HANDOFF p/ Sessão 17 (noite 2026-06-13, sessão autônoma)

## 🎯 MISSÃO: chefes/inimigos INVISÍVEIS nas fases — fazê-los aparecer.

## ⛔ BLOQUEADOR descoberto nesta sessão (LER PRIMEIRO)
A missão foi pedida "no s905x5m, por ser mais rápido". **NÃO É VIÁVEL no X5M sem trabalho grande**:
- O Cuphead (Unity) renderiza no **EGL REAL do Mali via fbdev** — a Unity cria o
  contexto/surface direto no `/dev/fb0` (`g_fbdev_win`, main.c ~530; `[F2] EGL REAL Mali fbdev`).
  O caminho SDL2/egl_shim é só fallback `CUP_SHIMEGL=1` e **a Unity não usa** (egl* é PLT → driver real).
- O **X5M é Mali-G310 Valhall / KMSDRM — NÃO tem EGL fbdev**. Logo o caminho default do Cuphead
  não inicializa lá. Subir no X5M = **portar o backend de display fbdev→kmsdrm** (rotear o EGL da
  Unity pelo SDL2/kmsdrm via CUP_SHIMEGL e fazer esse caminho funcionar) = sessão dedicada.
- **Conclusão:** o debug dos inimigos invisíveis tem que ser no **Mali-450 (.89, fbdev)** OU
  alguém porta o display backend pro X5M primeiro.

## ⛔ Estado dos devices nesta sessão
- Cuphead **NÃO está instalado em device nenhum** (.89 e .103 — storage reusado p/ Dysmantle/Bully;
  `/storage/roms/cuphead-recon` sumiu). Precisa REDEPLOY completo.
- Mali-450 (.89) = device certo p/ Cuphead (fbdev) MAS é o kernel-3.14 que **hard-wedge** sob
  pressão; sem ninguém p/ power-cycle de madrugada = não dá p/ rodar autônomo com segurança.
- Por isso NÃO rodei a fase nesta sessão (risco de travar o device a noite toda). Avançei só o
  que é seguro: rebuild do binário + análise + este handoff.

## ✅ Feito nesta sessão (seguro)
- Binário **rebuildado** com TODO o ferramental da s15 (ports/cuphead/cuphead, md5 d66e89103f39):
  CUP_NOSCISSOR, /tmp/dson+dsdump (SPROBE/MAT), CUP_ALPHAFIX, CUP_SHADERDUMP, CUP_TEXSTAT,
  EXTALPHA, watchdog. (build.sh OK; warnings de libSDL2 subsection são inofensivos.)

## 📦 RECEITA DE REDEPLOY (descoberta nesta sessão — layout dos assets)
Base hardcoded no binário: `ASSET_BASE_M = /storage/roms/cuphead-recon/` (main.c ~387).
Layout esperado (padrão Unity do APK):
```
/storage/roms/cuphead-recon/cuphead                         (binário)
/storage/roms/cuphead-recon/libil2cpp.so                    (de dump/ ou APK lib/arm64-v8a/)
/storage/roms/cuphead-recon/bin/Data/                       (= APK assets/bin/Data/*)
/storage/roms/cuphead-recon/bin/Data/Managed/Metadata/global-metadata.dat
/storage/cuphead-sa/                                         (saves; CUP_SAPATH)
```
Fontes: APK = ~/Downloads/Cuphead-DLC-v1.0.2-full-apkvision.apk (1.9G);
libil2cpp.so + global-metadata.dat = ~/cuphead-build/dump/.
Launcher de referência (Mali-450): ports/cuphead/go.sh (env CUP_* + SDL_VIDEODRIVER=mali).

## ❓ HIPÓTESES p/ os invisíveis (ordem; da s15 + nova)
Recap s15: draws SÃO emitidos (ext-alpha prog~24, atlas 2048 bound); ALPHAFIX (uniforms
_Color/_RendererColor→1, _EnableExternalAlpha→0) **NÃO resolveu**; player (prog22) no MESMO
fbo2 aparece. GL state dos invisíveis: colorMask 1111, blend ON, depth test=1/mask=0/LEQUAL.
1. **SCISSOR cortando** (não testado): `CUP_NOSCISSOR=1` já implementado → 1º teste.
2. **Matrizes zeradas** (geometria degenerada): [MAT] diag pronto via /tmp/dsdump.
3. **🆕 COR POR-VÉRTICE = 0** (lead novo desta sessão): como o ALPHAFIX mexeu só nos UNIFORMS
   e não resolveu, se o frag multiplica por uma varying de cor de vértice (COLOR0) que vem 0
   no caminho batched, os sprites somem mesmo com uniforms=1. **Análogo ao vertex-color branco
   do Dysmantle.** Teste decisivo: patch no shader forçando
   `gl_FragColor.rgb = texture2D(_MainTex,uv).rgb` (bypassa TODOS os multiplies de cor/vértice).
   Se aparecer → é a cadeia de cor (vértice/uniform); se continuar sumido → é geometria (1/2).
   ⚠️ precisa do TEXTO do shader ext-alpha (CUP_SHADERDUMP) p/ saber o nome da varying de uv —
   não tenho o dump salvo; capturar no 1º boot.

## ▶️ PRÓXIMO PASSO (s17), em ordem
1. Redeploy no **Mali-450 .89** pela receita acima (NÃO no X5M).
2. Conferir device (RO? swap? `cat /proc/swaps`; `mount|grep roms`).
3. `CUP_NOSCISSOR=1 ./go.sh` → navegar Forest Follies (reference_cuphead_navegacao_gpvirt) →
   screenshot. Apareceu? → promover NOSCISSOR a default, commit, FIM.
4. Não? → `touch /tmp/dson; touch /tmp/dsdump` na fase → ler [SPROBE] sciss/vp + [MAT] +
   capturar [SHSRC] do ext-alpha → decidir entre matriz (2) e vertex-color (3) → implementar →
   commit. Coletar RÁPIDO e matar (wedge risk; avisar p/ power-cycle).
