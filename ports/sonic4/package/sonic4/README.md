# Sonic The Hedgehog 4: Episode II - NextOS / PortMaster

This package contains only the Linux so-loader and launcher files. It does not
include game data.

## Required files

Copy your own Android game files into `roms/ports/sonic4/`:

- `sonic-the-hedgehog-4-episode-ii-2.0.0.apk`
- `cache-sonic-the-hedgehog-4-episode-ii-2.0.0.zip`

Instead of the cache ZIP, you may copy the OBB directly:

- `main.22.com.sega.sonic4episode2.obb`

On first launch the script extracts:

- `lib/armeabi-v7a/libfox.so` from the APK;
- `data/main.22.com.sega.sonic4episode2.obb` from the cache ZIP or direct OBB.

## Controls

Native gamepad is supported. A PortMaster `sonic4.gptk` fallback is included for
devices that route controls through gptokeyb.

- `A`: jump / confirm
- `B`, `X`, `Y`: game actions where supported
- `START`: pause in gameplay
- `SELECT+START`: exit

## Build

The included `sonic4` binary is armhf and built in Docker against old Debian
glibc symbols. The package intentionally does not bundle a `runtime/` directory.

Expected compatibility target: glibc 2.30+ devices.
