# Shantae and the Pirate's Curse — so-loader Mali-450 (.88) — STATUS

## ✅ FUNCIONANDO (2026-06-24 s1)
- **Boot completo**: so-loader ELF32-ARM próprio (so_util portado de aarch64→ELF32/REL/R_ARM_*).
- **IMAGEM**: logo WayForward animado + tela de TÍTULO renderizam (confirmado glReadPixels→PNG, 96.6% não-preto).
- **ÁUDIO**: OpenSLES, 0 underruns ([PERFAUD] active=1).
- **60 FPS travado** (16.7ms/frame, 0 frames >40ms).
- **INGLÊS** forçado (JNI Locale.getLanguage→"en").
- **INPUT chega ao jogo**: touch (MOTION) e botões (A=96, START=108) entregues em `onInputEvent` com `handled=1`.

## 🔑 FIXES-CHAVE (ordem)
1. **so_util ELF32-ARM**: Elf32, REL com addend implícito, R_ARM_RELATIVE/ABS32/GLOB_DAT/JUMP_SLOT, init_array /4. BUG crítico: pra GLOB_DAT/JUMP_SLOT NÃO somar o valor in-place (é stub do PLT, não addend) → `*ptr=S`. (ABS32/RELATIVE usam addend in-place.)
2. **struct android_app clássico 32-bit** (sizeof 148): offsets confirmados via disasm (window@36, pendingWindow@128, onAppCmd@4, onInputEvent@8, activity@12, mutex@64, cond@68, msgread@72, etc.). NÃO é GameActivity.
3. **g_pAndroidApp**: setado manualmente (`so_find_addr`) pois normalmente é setado em ANativeActivity_onCreate (que pulamos, vamos direto no android_main). Sem isso → LanguageGlobal crashava.
4. **stdio_shim**: `__sF` (stride bionic 84), ponte fwrite/fread/fputs/fputc/fseek/ftell/fclose bionic→glibc, tabelas `_ctype_`/`_toupper_tab_`/`_tolower_tab_` (ponteiros bionic).
5. **pthread_bridge**: + `pthread_create` ignora attr bionic (usa attr glibc 2MB stack); attr_* no-op. (bionic attr ≠ glibc attr → crash dentro do pthread_create.)
6. **__gnu_Unwind_Find_exidx** próprio: retorna o `.ARM.exidx` do módulo custom-loaded (vaddr 0x2e1198, size 0xc0b8) → exceções C++ desenrolam/capturam (sem isso, todo throw→terminate).
7. **softfp/hardfp ABI** (CAUSA DA TELA PRETA): jogo é **softfp** (Tag_ABI_VFP_args ausente, float em r0-r3), device/toolchain **hardfp** (VFP). `glClearColor` recebia (0,1280,0,720)=dims→tela preta. FIX=`softfp_shim.c` wrappers `pcs("aapcs")` p/ libm (sinf/cosf/powf/...) + `glClearColor`, roteados em `so_resolve` antes do dlsym.
8. **save**: criar dir `gamedata/` (parent), deixar o jogo criar o arquivo `gamedata/save` (NÃO criar como dir). Sem isso: SaveFile::GetState lança exceção.
9. **Analytics_Init** stubado (return 0): GetDeviceId fazia memcpy p/ buffer nulo (Google Analytics precisa Play Services).

## ✅ CONTROLES + NEW GAME + CUTSCENE CONFIRMADOS (s1, via glReadPixels)
Sequência completa navegada por input sintético (SHANTAE_AUTOKEY): logo WayForward →
título → splash "WayForward 25 Years" → **MENU de seleção de arquivo** (3 slots New Game,
ERASE/COPY/OPTIONS) → **New Game** → **cutscene de abertura** (mar/torre/Risky Boots,
legenda inglesa "Now is the time to...", áudio, 60fps). Platforming é logo após a cutscene.

10. 🔑 **INPUT (source EXATO)**: o engine Black roteia input por `getSource` ANTES de ler o
    keycode. Os 3 handlers exigem source EXATO: `Controller_impl::receiveMsg`→**0x501**
    (AINPUT_SOURCE_GAMEPAD 0x401 | KEYBOARD 0x101 = source REAL de botão gamepad Android),
    `Keyboard_impl`→0x301, `Touch_impl`→touch. Mandar 0x1000010(JOYSTICK)/0x401/0x701 = o
    handler bail sem ler keycode (input ignorado mesmo com onInputEvent handled=1).
    FIX: `push_key_event` usa source **0x501**. `OnInputEvent` empacota {deviceId,source,type,
    event_ptr} numa Message e BROADCASTA; o decode (getKeyCode/getAction) é no
    Controller_impl/Keyboard_impl subscriber. RepeatCount deve ser 0.

## 🎮 INPUT — MAPA COMPLETO DO ENGINE (disassembly, s2 2026-06-24)
O engine Black tem 3 subscribers do MessageSystem que recebem o broadcast do
`OnInputEvent` (que empacota {[0]=0xffffffff,[+4]=event_ptr,[+8]=deviceId,
[+12]=source,[+16]=type} e dá `Broadcaster::send`):
- **Controller_impl::receiveMsg** (0x24ed90): KEY exige `source==0x501`
  (GAMEPAD 0x401|KEYBOARD 0x101 = source REAL de botão gamepad Android) + type==1 +
  RepeatCount==0. keycode→bit: A96=0x10 B97=0x20 X99=0x40 Y100=0x80 R1(103)=0x100
  L1(102)=0x200 MENU(82)=0x400 BACK(4)=0x800 R3(107)=0x1000 L3(106)=0x2000.
  **START(108) NÃO mapeado → pause é MENU(82)**. MOTION exige `source==0x10`; lê
  getAxisValue dos eixos 0,1(Lstick) 11,14(Rstick) 15,16(HAT=dpad) 22,23(triggers).
- **Keyboard_impl::receiveMsg** (0x24f824): exige `source==0x301`; grava keystate
  (bitmask por keycode Android em this+48).
- **Touch_impl::receiveMsg** (0x24fb0c): touch.

### Estado confirmado
- ✅ Botão **A** (gamepad 0x501) funciona: avança título, confirma.
- ✅ SDL gera eventos OK p/ qualquer controle: A=btn0, dpad-down=btn12, JHAT, CAXIS.
- ✅ getAxisValue corrigido p/ softfp (retorna float em r0) — sem isto, drift "anda sozinho".
- ✅ Mandamos motion src 0x10 com Lstick certo; o jogo RECEBE (handled=1) — mas **NÃO move**.
- ✅ Mandamos dpad como teclado AKEYCODE_DPAD (src 0x301) → Keyboard_impl — **menu NÃO move**.

### ✅ CONTROLES RESOLVIDOS (s2 2026-06-24) — dpad + analógico, menu + gameplay
**RAIZ**: o branch MOTION do `Controller_impl::receiveMsg` (0x24ee84) exige
`msg.source == 0x01000010` (AINPUT_SOURCE_JOYSTICK), **NÃO 0x10** (a memória/STATUS
antigos diziam 0x10 — errado). Mandando 0x10 a engine fazia `bne` e nem lia os eixos
→ stick/dpad/triggers IGNORADOS mesmo com onInputEvent handled=1. (Era por isso que
"recebia mas não movia".) O HAT (axes 15/16) é convertido nos **4 bits baixos de
[ctrl+48]** = MESMA palavra de estado dos botões que já funcionavam → menu navega e
personagem anda.
**FIX** (android_shim.c): `SHANTAE_MOTION_SOURCE 0x10 → 0x01000010`. Além disso, o
analógico esquerdo é dobrado no HAT (threshold 0.5, dpad tem prioridade) → analógico
TAMBÉM navega/anda pela mesma nibble. Botões intactos.
**CONFIRMADO pelo NextOS no controle físico**: "deu certo no menu" + "jogo também".
Screenshot de gameplay (Shantae no nível do farol) OK. Binário md5 88ef1a0a.

## 🤝 HANDOFF P/ PRÓXIMA SESSÃO (s2 2026-06-24)

### ✅ FUNCIONA (confirmado pelo NextOS no controle FÍSICO)
- Boot, render (logo/título/menu/cutscene/**gameplay com a Shantae no nível**), áudio, 60fps, inglês, 0 crash.
- **Botões**: A confirma, START pausa (MENU), e os outros respondem — via Controller_impl (source 0x501).
- **Drift "anda sozinho" RESOLVIDO**: getAxisValue agora é softfp (pcs aapcs) → não anda mais sozinho.

### ❌ FALTA SÓ: MOVIMENTO DIRECIONAL (andar/navegar menu com dpad+analógico)
Nem o menu (trava no slot 1) nem o personagem andam com dpad/analógico. Botões OK, movimento não.

### O QUE JÁ FOI TENTADO (e NÃO resolveu o movimento)
1. dpad/stick como **gamepad-axis** (Controller_impl, motion source 0x10, eixos 0/1/11/14/15/16/22/23): o jogo RECEBE (handled=1) mas não move.
2. dpad/stick como **teclado** AKEYCODE_DPAD_* (Keyboard_impl, source 0x301, keystate): menu não move.
(ambos os caminhos estão NO BINÁRIO atual — `push_kbd_event` src 0x301 + `push_joystick_event` src 0x10 + hat.)

### 🎯 PRÓXIMO PASSO (não adivinhar — achar o reader)
O Controller_impl (0x24ed90) lê o HAT assim: axis15(HAT_X)→`vmov s16,r0`; axis16(HAT_Y); depois faz `vcmpe.f32 s16,#0.0` e `vcmpe.f32 s0,s2` (compara hat/stick com threshold) → provavelmente converte em bits de direção num campo do estado do controller (this+...). **Achar onde o jogo LÊ esse estado de direção** (o player-movement e o menu-cursor): rastrear os readers do struct do Controller_impl (r4+48 = button bitmask; os eixos vão em r6+0..20) e do keystate do Keyboard_impl (this+48). Decidir se o menu é **TOUCH** (emitir Touch_impl nas coords dos 3 slots) ou se há um keycode/bit específico de direção que falta.
- Ferramentas: gdb no .88 (break nos readers), disassembly (objdump force-thumb), `SHANTAE_INDBG=1`/`SHANTAE_INLOG=1` logam SDL + getKeyCode/getSource. Screenshot: `touch /dev/shm/dys_shot` → `/dev/shm/dys_shot.raw` (RGBA 1280x720, flip V); puxar com `ssh "cat" > local` (scp trava).
- ⚠️ NUNCA deixar autokey ligado quando o NextOS for testar (SHANTAE_AUTOKEY/KBDTEST/AUTODPAD/AUTODPAD) — atrapalha o teste físico dele. O launcher Shantae.sh NÃO seta nenhum (limpo).
- 🔑 Princípio do NextOS: controle-agnóstico = SDL padrão Xbox + gptokeyb→teclado (como os outros ports). O jogo TEM Keyboard_impl (teclado) e Touch_impl.

## 📦 EMPACOTAMENTO (s2 2026-06-24, em andamento)
- **Launcher PortMaster padrão** `Shantae.sh` (ports_scripts) reescrito: source control.txt,
  get_controls, mata stale por /proc/*/exe (regra #3), trap cleanup (mata shantae+gptokeyb,
  restart ES, pm_finish), **NÃO força SDL driver** (regra #6), roda `$GPTOKEYB "shantae" -c
  shantae.gptk &` (fallback /usr/bin/gptokeyb), depois `./shantae`. gptokeyb é best-effort:
  o jogo roda 100% no SDL nativo mesmo sem ele.
- **shantae.gptk**: back=esc start=enter (=> SELECT+START fecha via teclado também),
  a=x b=c x=q y=t l1=h r1=j l2=k r2=l l3=n r3=m, dpad+analógico=setas.
- **Spam de log silenciado**: pb_try_connect desiste após 1ª falha (Shantae não tem
  Paddleboat). Binário novo md5 04a1e742 (controles idênticos ao 88ef1a0a, só cosmético).
- DEPLOYADO no .88: Shantae.sh (ports_scripts) + shantae.gptk (ports/shantae). FALTA
  (precisa janela com jogo fechado): deployar binário 04a1e742 (ETXTBSY) + TESTAR que
  gptokeyb NÃO faz grab exclusivo (senão quebraria o nativo) + confirmar SELECT+START.

## ⏳ FALTA
- Deploy binário final + teste end-to-end do launcher (gptokeyb não-grab + SELECT+START).
- Commit master (binário NÃO versionado; versionar src/ + Shantae.sh + shantae.gptk + STATUS.md).

## COMO RODAR / DEBUG
- Build: `./build.sh` (toolchain armv7 hardfp Amlogic-old).
- Device .88 (EmuELEC), dados em `/storage/roms/ports/shantae/` (binário+lib/+assets/ 427MB .vol).
- Launcher: `Shantae.sh` (PortMaster; NÃO força SDL driver).
- Screenshot do que o jogo desenha: `touch /dev/shm/dys_shot` → próximo swap salva `/dev/shm/dys_shot.raw` (RGBA 1280x720, flip vertical).
- Envs: SHANTAE_ASSETS, SHANTAE_AUTOKEY=1 (auto-input teste), SHANTAE_GLLOG=1, DYSMANTLE_DEBUG=1 (verboso).
- ⚠️ matar shantae por /proc/*/exe antes de scp (ETXTBSY) e antes de lançar (rule: 0 instâncias).
