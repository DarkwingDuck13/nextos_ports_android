# DYSMANTLE — NextOS / PortMaster (ARM64)

Native AARCH64 so-loader port (v6). No game data is distributed with this
port — it uses YOUR legal Android copy of DYSMANTLE.

> DYSMANTLE is a paid 10tons title on Android. This port does NOT include
> the game and does NOT bypass any purchase. You provide your own legal copy.

Required version: **1.4.1.12** (arm64-v8a). Other versions are not supported.

------------------------------------------------------------------------
## 1. Get the game files — complete APK

Copy your DYSMANTLE **APK v1.4.1.12** (single file, ~800 MB) to:

    roms/ports/dysmantle/

That is all — the first launch shows the SETUP screen and extracts
everything from it (the APK is deleted after a successful setup).

No PC? Use "SAI (Split APKs Installer)" on your phone to export the APK
of your installed copy, then copy it over.

------------------------------------------------------------------------
## 2. First launch — setup stages

The first run shows a setup screen with progress:

    STAGE 1/4  game library
    STAGE 2/4  extracting data (~734 MB, live MB counter)
    STAGE 3/4  texture repair (a few minutes)
    STAGE 4/4  texture optimization (ONLY on some low-memory devices —
               this one is SLOW, up to ~15 min. Do not power off!)

The game opens automatically when setup ends. This happens only once.

------------------------------------------------------------------------
## 3. DLCs (The Underworld / Doomsday / Pets and Dungeons)

If you OWN DLCs on Android, copy your Android SAVE (with DLC progress) to:

    roms/ports/dysmantle/gamedata/10tons/DYSMANTLE/save/0/

Only the DLCs your save proves you own are unlocked. Nothing you do not
own gets unlocked.

------------------------------------------------------------------------
## 4. Controls

Standard gamepad. **SELECT + START = quit** back to the menu.

------------------------------------------------------------------------
## 5. Devices / tech notes

- One universal binary (glibc >= 2.27): ArkOS, dArkOS, ROCKNIX, EmuELEC,
  NextOS, muOS, Knulli and other aarch64 CFWs.
- Low-memory devices (R36S class) with swap/zram use NATIVE-quality
  texture streaming; without swap a texture-optimization cache is baked
  once at setup (stage 4/4).
- GLES2 and GLES3 GPUs supported (automatic).

Problems? Delete `roms/ports/dysmantle/assets/` and the hidden markers
(`.textures_fixed`, `.etc1_scale`), put the APK back and launch again to
redo the setup.
