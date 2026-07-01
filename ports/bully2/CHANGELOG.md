
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
