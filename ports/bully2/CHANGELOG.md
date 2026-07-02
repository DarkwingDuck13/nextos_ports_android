
## 2026-07-01

- Controles no caminho SDL nativo (devices sem gptokeyb): L1=mira, R1=tiro,
  L2/R2=troca de item, L3/R3=funcoes de stick (antes mira/tiro caiam em L3/R3).
- Threads do engine em cores dedicados em devices >=4 cores; BULLY2_THREAD_PIN=0 desliga.
- Slots JNI estaticos e de field corrigidos (antes retornavam 0 silencioso).
- Menu de pause volta ao layout completo (Inventory/Upgrades diretos);
  BULLY2_FORCE_PHONE=1 opta pelo layout phone (hub Info).
- RAM reportada ao engine auto por /proc/meminfo (BULLY2_DEVICE_RAM_MB override).
- BULLY2_FPS_LIMIT=15..120 opcional.
- Zip v11 p/ testers regenerado (sha256 6c1288e4cef8...c0fcc8f31b).
- Multi-CFW (estudo 4 agentes): escada de config GL ES2/ES3 + depth 24/16 com
  rejeicao de desktop-GL, present por lista positiva, preload versionado,
  pin de threads so em CPU homogenea, OpenAL-soft 1.21.1 bundlado + mute-stubs.
  Validado Mali-450 (principal, cutscene renderizada) e R36S. Zip v11 sha256
  397bf74f...662331c1.
- Hotfix X5M: driver SDL comparado case-insensitive no present (KMSDRM
  maiusculo do EmuELEC novo dava tela preta). Zips v11 regenerados.
