# Sonic The Hedgehog 4: Episode II - NextOS / PortMaster

This package contains only the Linux so-loader, launcher and PortMaster metadata.
It does not include game data.

## What's new in v4.1

- **Full ArkOS support (R36S and other RK3326/Mali devices):** the launcher now also
  looks in `/usr/local/lib/arm-linux-gnueabihf` for the device GLES/EGL/libmali libraries,
  so the game loads and renders on ArkOS (where these live outside the default library
  path). Validated on an ArkOS 2.0 R36S (libmali Bifrost G31): first-run extraction,
  video, audio and gameplay all working.

- **Fixed the Electric Road stage (Episode Metal / casino with Metal Sonic):** the
  background and spotlight beams blew out to solid white and on-screen objects
  (rings, springs, Sonic's jump) smeared/duplicated. An off-screen background buffer
  that the engine expects cleared every frame was accumulating. It is now cleared
  per-frame; the stage renders correctly. (Disable with `SONIC_NO_CLEARALL`.)
- **Fixed crash when leaving a stage** (Start -> Return to Stage Select closed the
  game): the deferred texture-release ran against scene data that had already been
  freed. The release path is now validated and safe. (Disable with `SONIC_NO_RELSAFE`.)
- **Device compatibility (some CFW):**
  - *No video on Mesa/Panfrost devices (e.g. ROCKNIX):* the GL driver now requests a
    GLES/EGL context so the game's GLSL ES shaders compile (was getting a desktop-GL
    context -> black screen). Disable with `SONIC_NO_FORCE_GLES`.
  - *Sound coming out of HDMI instead of the speaker (e.g. muOS):* audio now prefers
    the speaker (skips HDMI and retries the busy speaker card first). Disable with
    `SONIC_NO_PREFER_SPEAKER`.

## What's new in v4.0

- **Fixed gamepad button mapping** (root cause): the pad bit map now matches the
  game's own keycode table (`s_remapKey`). Previously the **Y** button sent the
  L1 bit, so Y acted like a page/left scroll in the world map and its real menu
  function never triggered. Y, X, L1/L2/R1/R2, Select and Start now use the
  correct bits.
- **Fixed audio volume on ROCKNIX / Knulli** (and other raw-ALSA CFW): when the
  system falls back to the raw audio card, the volume now follows the system
  volume (reads `system.cfg` / `batocera.conf`, with an `amixer Master` fallback)
  instead of staying at full volume.
- L3/R3 (stick clicks) no longer inject spurious pause/confirm.
- **Full visual effects on by default** (bloom + object shadows). The previous
  low-FX default caused visual issues on some devices. To trade visuals for
  performance on weak GPUs, set `SONIC_LOWFX=1` (or `SONIC_NOBLOOM` /
  `SONIC_NOSHADOW` for fine control).
- New cover art.

## Required files

Copy your own Android game files into `roms/ports/sonic4ep2/`:

- `sonic-the-hedgehog-4-episode-ii-2.0.0.apk`
- `cache-sonic-the-hedgehog-4-episode-ii-2.0.0.zip`

Instead of the cache ZIP, you may copy the OBB directly:

- `main.22.com.sega.sonic4episode2.obb`

On first launch the script extracts:

- `lib/armeabi-v7a/libfox.so` from the APK;
- `data/main.22.com.sega.sonic4episode2.obb` from the cache ZIP or direct OBB.

The zip uses the classic PortMaster layout: `Sonic4EP2.sh` at the zip root and
the `sonic4ep2/` data folder beside it. Install/extract it into `roms/ports`.
First-run extraction uses a progressor window when available and falls back to
plain log output otherwise.

## Controls

Native gamepad is supported. A PortMaster `sonic4.gptk` fallback is included for
devices that route controls through gptokeyb.

- `A`: jump / confirm
- `B`: cancel / back
- `X`, `Y`: their native in-game / menu functions (fixed in v4.0)
- `START`: pause in gameplay / confirm on title
- `SELECT+START`: exit

## Build

The included `sonic4` binary is armhf and built in Docker against old Debian
glibc symbols. The package intentionally does not bundle a `runtime/` directory.

Expected compatibility target: glibc 2.27+ armhf-capable devices.
