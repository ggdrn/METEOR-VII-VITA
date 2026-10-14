# FINAL FANTASY VII — PS Vita

A *loader* that runs the native library of the Android version of FINAL FANTASY VII
(v1.0.29) on the PlayStation Vita, without emulation: the original ARMv7 `.so` runs
directly on the CPU, with the Android system calls redirected to their Vita equivalents.

> **No game data is distributed.** You need to have bought the game on
> Google Play and copy the files from your own installation. See [docs/DATA.md](docs/DATA.md).

Status and roadmap: [PLANO.md](PLANO.md).

## Build

Requirements: macOS or Linux x86_64, `cmake`, `git`, VitaSDK (hardfp) in `~/vitasdk`
(used only as a source of up-to-date headers on macOS).

```sh
git clone --recurse-submodules <repo> && cd <repo>
scripts/setup-softfp-sdk.sh        # installs VitaSDK-softfp in ~/vitasdk-softfp
source scripts/env.sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8            # produces build/ff7_vita.vpk
```

The build ends with `scripts/check-vpk.sh`, which fails if the VPK contains any
file other than the loader and the LiveArea.

Useful targets (set `-DPSVITAIP=<ip>`): `send`, `send_kvdb`, `dump`, `reboot`.

## Layout

```
source/main.c        main loop and controls
source/dynlib.c      .so import table → native Vita functions
source/java.c        reimplemented Java methods (FalsoJNI)
source/patch.c       game-specific patches
source/reimpl/       reimplementations of Android APIs (AAsset, EGL, pthread...)
source/utils/zip.c   reader for the original APK/OBB, without extraction
lib/                 so_util, FalsoJNI, vitaGL (submodules), kubridge, libc_bridge
scripts/             SDK setup, environment, VPK check, address table (gen-memmap-sites.py)
```

## Credits

- Base: [soloader-boilerplate](https://github.com/v-atamanenko/soloader-boilerplate) by Volodymyr Atamanenko (gl33ntwine)
- [TheFloW](https://github.com/TheOfficialFloW): original so-loader, gtasa_vita
- [Rinnegatamante](https://github.com/Rinnegatamante): vitaGL, so_util, vitaShaRK
- [fgsfds](https://github.com/fgsfds): various contributions

FINAL FANTASY VII © SQUARE ENIX. This project is not affiliated with Square Enix.

## License

MIT. See [LICENSE](LICENSE).
