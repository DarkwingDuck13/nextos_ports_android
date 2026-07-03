# Mega Man X port runs

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
