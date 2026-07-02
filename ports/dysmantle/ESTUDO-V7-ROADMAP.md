# ESTUDO — DYSMANTLE v7: o que fazer nas próximas versões (2026-07-02)

Estado de partida: **v6 FINAL** (zip sha256 f2fdc1c7...) — streaming de textura com guardas
anti-OOM/anti-piscada, ES-off em low-RAM, preset Low da engine na classe <700MB, tela de
bake em etapas, gate GLIBC<=2.30, READMEs EN/pt-BR. Validado em 5 devices.

## 1. Validar e AFINAR o preset Low (curto prazo, alto valor)
- `dys_force_detail` chama `Shadegrown::SetDetailLevelPreset` no singleton exportado
  `shadegrown` — APLICOU sem crash, mas falta MEDIR: curva de RAM (mundo anon) e fps
  com Low vs sem, e o veredito VISUAL do usuário em gameplay real.
- Se Low for agressivo demais no visual: testar "Medium"; ou usar os setters INDIVIDUAIS
  que a engine exporta (`Shadegrown::SetShadowDetailLevel(int)`,
  `ParticleEffectManager::SetDetailLevel`) p/ cortar SÓ sombra/partícula e preservar o resto.
- `Shadegrown::ApplyOptions(bool)` existe — investigar se consolida as opções após o set.

## 2. O crescimento do MUNDO (o limite real dos 639MB) — RE de médio prazo
Fato medido: mundo cresce ~350MB anon em ~20min de gameplay (OOM aos 22min sem preset).
Textura NÃO é o problema (pool ~14MB). Alvos de RE:
- `StageObjectAllocatorPage` / "General Pool": existe descarte de ZONA ao sair dela?
  Se existe e não roda no nosso caminho, achar o gatilho (igual fizemos com o preset).
- `Shadegrown::OnEnterMainMenu()` — o que ele LIBERA? Talvez exista um "flush" chamável
  em transição segura (porta do submundo/fast travel = tela de load natural).
- Comparar RSS pós-load de save vs pós-exploração: leak ou working-set legítimo?

## 3. Multi-CFW de verdade (testers externos)
- muOS / Knulli (PipeWire), TrimUI (PowerVR: escada GL d16 já pronta — validar shaders),
  RGCubeXX (720x720 — conferir layout/letterbox), dArkOS. Matriz do bully2 como guia.
- X5M: revalidar caminho ES3/ETC2 nativo com o binário final (última validação foi 109fps).
- R36S cabeado: enviar binário final (ficou offline com build antigo) — 1 scp.

## 4. Qualidade/fps de gameplay no RK3326 (15-20fps)
- Medir com preset Low ativo (deve subir: menos partícula/sombra = menos drawcall).
- Se precisar mais: perfilar drawcalls por frame (DRAWSTATS já existe); avaliar
  DYSMANTLE_ISCALE_AUTO com limiar menor SÓ se o usuário aceitar o trade nitidez/fps.

## 5. Streaming — upgrades adiados (só se aparecer necessidade)
- `glTexSubImage2D` rect-no-swap (§2.3.2 do estudo mestre): até agora `sub=0` em todos os
  devices — implementar apenas se algum CFW/cena acusar atlas dinâmico.
- Cache persistente id-keyed entre sessões: id GL muda por run — NÃO tentar name-keyed
  (lição Bully fase 3: textura errada).

## 6. Distribuição
- Subir o zip v6 pro R2 (quando o usuário pedir) + post pros testers (changelog pronto).
- Avaliar submissão PortMaster oficial (port.json/gameinfo já no padrão; BYO-APK ok).
- Menu de opções na instalação (estilo SOR4): escolher qualidade de textura/preset na
  tela de bake (o setup_splash já desenha; falta input no splash).

## 7. Pendências pontuais
- Cartão do R36S wifi antigo (.220) pode ter fixpak quebrado (GLIBC_2.34) — se voltar,
  rodar o fixpak bom + marker (2 min por ssh).
- gptokeyb não sobe no ArkOS (check antigo em outros CFWs cobre; nativo funciona) — ok.
- `DYSMANTLE_FORCE_NATIVE_ETC2` no R36S: segue proibido (crash ground-tiles, muro antigo).

## Regras permanentes (não esquecer)
- TODO binário shipável ≤ GLIBC 2.30 — `check_glibc.sh` no build e `make_zip.sh` como
  ÚNICO empacotador (gate duplo). Zip manual NUNCA.
- essway (ROCKNIX/ArchR) nunca parar; zram nunca mexer (lado do sistema).
- Kill de jogo por /proc/*/exe (comm=Main; sob ES-off roda como root — usar ps ax).
