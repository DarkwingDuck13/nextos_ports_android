# Mega Man X port runs

## 2026-07-04 button action study: A/Y did not become playable yet

### Current state after study

- Device and repo were restored to the stable s8 gameplay/audio checkpoint.
- No `megamanx` process was left running on `192.168.31.79`.
- Stable hashes on device after restore:
  - `megamanx`: `5ecffe70c84c34e8aa6f6a851997232626845eda96348d452f2319973c9317bc`
  - `run.sh`: `42dd1639693645ab39df778ec7a06c5863c677c8ed5b1a58653787d36fc645e3`

### What was tested

- Explicit `MMX_CTRL_BTN_JUMP=0`: no effect; this was already the default.
- `A -> KEYCODE_SPACE(62)`: no effect.
- Hybrid native gamepad plus touch-only actions (`A` jump, `Y` shot, `START` pause): no effect.
- `MMX_CTRL_REAL_HELD=1`: bad default, caused continuous dash behavior.

### Key evidence

- Physical buttons reached the game as KeyEvents:
  `A=96`, `B=97`, `X=99`, `Y=100`, `LB=102`, `RB=103`.
- `B` dash still works through the native Android/Unity path and should be preserved.
- `MMX_CTRL_FORCE_IDX=2` makes X jump/float, proving `game_key[2]` is a working jump action through
  the direct `RockmanX.controlKey` path.

### Saved run

- `runs/2026-07-04-codex-forceidx-study`
  - `force_idx_2.png`: X is airborne while `MMX_CTRL_FORCE_IDX=2` is forced.
  - `force_idx_2.ppm`: original raw screenshot.
  - `force_idx_2.run.out`: log for the forced-index run.
  - PNG SHA256: `46c70129621eb8e71e0040c55e9441a3b1136041861ff0de1d1a22f743aff3fa`
  - Log SHA256: `ee547770f5c16bfa0e4f91681e87a4c1b1c39bc13511aa1b3041c09b3596de12`

### Resume point

Do not retry global `MMX_CTRL_REAL_HELD=1`. The next likely fix is an edge-triggered short pulse:
on physical `A` down, inject `game_key[2]` into the real/trigger planes for 2-3 frames only, then release.
Find the shot index with a `MMX_CTRL_FORCE_IDX=N` sweep before mapping `Y`.

## 2026-07-04 audio ForceSL + stream fallback

### Current state

- Full version, auto-start gameplay, physical pad, and audio are now enabled by default in `run.sh`.
- Audio fix is two-stage:
  - `MMX_FORCESL=1` forces Unity/FMOD output 22 (OpenSL) instead of output 21 (AudioTrack/Java fake).
  - `MMX_STREAMFALLBACK=1` retries failing FMOD streams (`mode=0xd2 -> 33`) as samples (`mode=0x52 -> 0`).
- Remaining major blocker: controller navigation for touch-only submenus.

### Saved run

- `runs/2026-07-04-codex-audio-forcesl-fallback`
  - `run.out`: clean 45s validation log.
  - `audio_forcesl_fallback.png`: gameplay screenshot after audio fix.
  - Screenshot SHA256: `2dea0c00f899aec7aa23ee8c9de416a421a9df7c2b91125318ed8fc93fd02a2b`.

### Key evidence

- OpenSL path reached: `[SL] slCreateEngine`, `CreateOutputMix`, `CreateAudioPlayer`, `bq_Enqueue`,
  `SetPlayState`, pump callbacks.
- FMOD stream fallback worked: `STREAM falhou(33) -> retry sample mode=0x52 -> 0`.
- `Cannot create FMOD::Sound` count in the clean validation: `0`.
- Gameplay/control still alive in the same run: `GOSTAGE`, `CTRLHOOK`, and `MMX_GAMEPAD` logs are present.

### Current playable recipe

```sh
sh /storage/roms/megamanx/run.sh
```

Important envs inside `run.sh`:

```sh
MMX_INLINETASK=1 MMX_PATCH=0x34eafc=0x14000005
MMX_NOINTEGRITY=1 MMX_PREFSTRUE=1 MMX_FIXGAME=1 MMX_FULLVER=1
MMX_XLATE=1 MMX_BOOTST=1
MMX_FORCESL=1 MMX_STREAMFALLBACK=1
MMX_GAMEPAD=1 MMX_CTRLHOOK=1 MMX_CTRL_KEYFLAG_PRE=1 MMX_KEYINIT=1
MMX_GOSTAGE=0 MMX_GOSTAGE_F=280
```

## 2026-07-03 pause checkpoint

User asked to pause. No `megamanx` process left running on `192.168.31.79`.

### Current state

- Rendering is still good: X and highway stage draw cleanly.
- `MMX_FULLVER=1` ProductInfo hooks are active; forced-stage runs no longer show the "BUY FULL VERSION" gate.
- Native Xbox path is real: Unity/InputSystem sees `XboxOneGamepadAndroid`, accepts `nativeInjectEvent` with `ret=1`, and queries axes.
- Direct `RockmanX.controlKey` hook is installed and sees gamepad state.
- Current blocker: `key_data` and `game_key` arrays remain length 0 in the forced-stage flow, so direct keymap injection has no source masks to OR into.
- Audio blocker unchanged: FMOD keeps logging `Cannot create FMOD::Sound instance...`.

### New saved runs

- `runs/2026-07-03-codex-native-xbox-keep-controlkey`
  - Validated native Xbox path with `MMX_KEEP_CONTROLKEY=1`.
  - Events accepted (`ret=1`); game reached stage.
- `runs/2026-07-03-codex-ctrlhook-direct-stage`
  - First direct `MMX_CTRLHOOK=1` test.
  - `controlKey` hook installed, but early logs showed `key_data len=0` / `game_key glen=0`.
- `runs/2026-07-03-codex-fielddump-rockmanx-input`
  - Metadata dump confirmed `RockmanX` offsets:
    `KeyFlag +0x2c0`, `KeyFlagReal +0x2c8`, `key_data +0x2d0`,
    `def_key +0x2d8`, `game_key +0x2e0`, `touchkey_tbl +0x2e8`,
    `flg_touchKey +0x2f0`, `flg_touchPress +0x300`, `flg_touchRelease +0x308`.
- `runs/2026-07-03-codex-ctrlhook-actlog-pause`
  - Last run before pause.
  - Key proof in `debug.log`: `CTRLHOOK enter ... gp=0x2000 act=0x8`, then
    `CTRLHOOKKD miss ... key_data len=0 ... game_key ... glen=0`.
  - `shot.png` saved; stage still renders correctly.

### Resume target

Do not restart from shader/fullver. Resume at controls:

1. Find where `key_data/game_key` are initialized in the real menu/start flow or call that initializer before `scn_STAGE`.
2. If that is slow, inject into `flg_touchKey` / `flg_touchPress` / `flg_touchRelease` directly, using SDL pad state.
3. Keep `MMX_KEEP_CONTROLKEY=1` or `MMX_CTRLHOOK=1` for every control test.

## 2026-07-03 Codex session

Initial state from old `HANDOFF.md`: game booted on `.79`, Play Integrity was bypassed, shaders compiled, but the framebuffer stayed magenta / old diagnostics said `draws/f=0`.

### Result summary

- Old `draws=0` wall is resolved.
- Real Mega Man X title screen renders on the Mali-450.
- Text/TMP magenta is resolved by aliasing MMX TMP shaders to Terraria `Hidden/TextCore/Distance Field SSD`.
- Android `MotionEvent` injection reaches Unity input parsing, but the title screen does not accept it yet.
- Temporary scene advance works with `MMX_FORCE_TITLESTART=1`; live device reached a menu/tela saying `Buy Full Version`.
- Current blockers: full-version/IAP gate, then controls, then FMOD audio clips.

### Important runs

- `runs/2026-07-03-codex-shader-tmp-textcore-alias`
  - Good visual reference.
  - `data.unity3d.tmp-textcore-alias` / deployed `payload/assets/assets/bin/Data/data.unity3d`
    SHA256 `197d3481015ae047e47d424be8a8bb3d73562e6465c0092581166767bdaf0edc`.
  - `shot.png` SHA256 `4c9da24dca2a5a8ada814cafaf50b4eed5c6ea16a60b495f64e4b5644ecffacc`.
  - Magenta pixels: 0.
- `runs/2026-07-03-codex-autotouch-start900-touchlog`
  - Proved injected `MotionEvent` enters Unity: `getPointerCount`, `getActionMasked`, `getX`, `getY`.
  - Title did not advance by touch alone.
- `runs/2026-07-03-codex-keep-controlkey-autotouch`
  - `MMX_KEEP_CONTROLKEY=1` left `RockmanX.controlKey` active without immediate crash.
  - Still no title advance by touch alone.
- `runs/2026-07-03-codex-force-titlestart`
  - Env included `MMX_FORCE_TITLESTART=1 MMX_KEEP_CONTROLKEY=1 MMX_RXSPY=scene`.
  - Patch: `scn_TITLE_run+0x20c` at `libil2cpp+0xdf53d0`, original `cbz x0, wait`, changed to `NOP`.
  - RXSPY confirmed `scn_TITLEMENU_run/draw` after title.
  - User saw `Buy Full Version` on device.
  - Log has FMOD spam for clip `"114"`.

### Stable visual recipe

```sh
sh /storage/roms/megamanx/dbgrun.sh 45 \
  MMX_INLINETASK=1 MMX_PATCH=0x34eafc=0x14000005 \
  MMX_NOINTEGRITY=1 MMX_PREFSTRUE=1 MMX_FIXGAME=1 \
  MMX_BOOTST=1 MMX_XLATE=1 CUP_DRAWCOUNT=1
```

### Force menu recipe

```sh
sh /storage/roms/megamanx/dbgrun.sh 60 \
  MMX_INLINETASK=1 MMX_PATCH=0x34eafc=0x14000005 \
  MMX_NOINTEGRITY=1 MMX_PREFSTRUE=1 MMX_FIXGAME=1 \
  MMX_KEEP_CONTROLKEY=1 MMX_FORCE_TITLESTART=1 MMX_RXSPY=scene \
  MMX_BOOTST=1 MMX_XLATE=1 CUP_DRAWCOUNT=1
```
