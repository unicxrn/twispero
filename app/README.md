# app: the PS5 side of Twispero

This folder is a fork of [EVO Player](https://github.com/sainsaji/EVO-PLAYER-PS5) by sainsaji, which grew out of [ProsperoPlayer](https://github.com/KINGDKAK/ProsperoPlayer) by KINGDKAK. EVO does the hard parts: hardware video decoding, audio, the UI and the packaging. The [top-level README](../README.md) covers what the app does and how to build it.

The fork keeps EVO's player and drops what a Twitch app doesn't use: the file browser, FTP and network shares, Live TV and the media servers, typing from a phone, subtitles, Surround Studio, Media Info, the upscaler, motion smoothing, Deep Blacks, the HDR, 120 Hz and frame-rate settings, and audio passthrough. In their place it has the Twitch screens, a quality picker and a Twitch-style player overlay.

## What's where

- `projects/evoplayer/`: the app. `sce_sys/param.json` holds its name and title ID, `VERSION` its version, and `NOTICE` where the code comes from.
  - `twitch/`: the glue between the player and the Twitch core in `../core`
  - `core/src/screens/Twitch*` and `ui_kit/src/evo_hui_twitch_*`: the Twitch screens
  - `media/` and `pp/`: decoding, audio, and putting frames on screen through `sceAgc`
  - `ui_kit/` and `ui_rml/`: the screens drawn with the UI kit, and the RmlUi pages the app falls back to
  - `assets/`: fonts, icons, sounds and pages, built into `eboot.bin`. Only the licence texts ship as files.
  - `shaders/agc/`: the GPU pipelines, compiled ahead of time (see `tools/build_agc_pipes.py`)
- `scripts/`: build, package, release and deploy.
- `tools/`: what the build runs (the app-module tools in `native-app/`, the asset bundler, the CA roots, the app's identity, the zip packer) and the generators for the logo, icons and shaders.
- `third_party/`: the UI kit and the FFmpeg source.
- `Dockerfile` and `docker-compose.yml`: the build image, pinned to Ubuntu 24.04, LLVM 18, PS5 Payload SDK v0.42 and pacbrew v0.39.

## Rules worth knowing

- Build with the scripts, in the container. Don't call `make` yourself: the Makefile doesn't list FFmpeg's own dependencies, so `package-app.sh` and `build-evoplayer.sh` do the linking.
- Run one build at a time. Builds share the SDK, FFmpeg and ccache volumes.
- Never copy a new build over a running app. Quit it from the side menu, wait for "safe to close", close it with the PS button, and only then deploy.
- `--usb-remote` and `--breadcrumbs` in `package-app.sh` are for development builds. Release builds leave them out.

## Credits

EVO Player is licensed GPL-3.0-or-later, like ProsperoPlayer before it ([COPYRIGHT.md](COPYRIGHT.md), [NOTICE](projects/evoplayer/NOTICE)). EVO's own thanks go to:

- [ps5-payload-dev](https://github.com/ps5-payload-dev) (John Törnblom), for the SDK and toolchain
- [KINGDKAK](https://github.com/KINGDKAK), for ProsperoPlayer
- [zecoxao/sce_symbols](https://github.com/zecoxao/sce_symbols), for the NID symbol database
- [BlackBearReloaded](https://github.com/blackbearreloaded), for [ps5-homebrew-ui](https://github.com/blackbearreloaded/ps5-homebrew-ui), the UI kit the screens are built on
- [mihawk-99](https://github.com/mihawk-99), for [PS5_VulkanTemplate](https://github.com/mihawk-99/PS5_VulkanTemplate), whose 1 GiB flexible-memory change the app module uses
- the PS5 developers Philow, sword and 0xManuel, for their help and research
