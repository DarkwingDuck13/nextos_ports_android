# crazytaxi-aarch64

Port of Crazy Taxi Classic (Android aarch64) to Linux aarch64

> **Adaptação NextOS / Mali-450 (Utgard).** O loader original é de
> **[initdream/crazytaxi-aarch64](https://github.com/initdream/crazytaxi-aarch64)**,
> construído em cima do framework
> [nextos_ports_android](https://github.com/felc18-blip/nextos_ports_android).
> Aqui ele foi **adaptado para o Mali-450 (fbdev)**: recompilado no toolchain
> NextOS Amlogic-old, com mapeamento teclado→keycode Android para **gptokeyb**
> e ajustes de áudio (PulseAudio). **Só o código vai pro repo** — os dados do
> jogo (copyright Sega) são BYO (you bring your own).

## Screenshots

<img src="images/muOS_20260615_2049_2.png" width="49%"> <img src="images/muOS_20260616_0257_0.png" width="49%">

## Requirements

- An aarch64 environment
- Game files from a legitimate Android copy of Crazy Taxi Classic

## Build

```bash
mkdir build
cd build
cmake ..
make
```

## Run

```bash
./crazitaxi
```

## Credits

* **[max_arm64](https://github.com/orktes/max_arm64)** by [@orktes](https://github.com/orktes)
  * For the proof of concept of the .so loader architecture.

* **[lswtcs_arm64](https://github.com/mtojek/lswtcs_arm64)** by [@mtojek](https://github.com/mtojek)
  * For some useful files and general structure.

* **[nextos_ports_android](https://github.com/felc18-blip/nextos_ports_android)** by [@felc18-blip](https://github.com/felc18-blip)
  * For the framework and template to build off of.

* **[BinaryCounter](https://github.com/binarycounter)**
    * For showing a prototype of this game working, giving me the opportunity to research the so-loading technique that resulted in this port.

## Legal

You must own a legitimate copy of the game to use this port.
