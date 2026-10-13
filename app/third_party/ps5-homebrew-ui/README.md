# ps5-homebrew-ui (library only)

This is the library part of [ps5-homebrew-ui](https://github.com/blackbearreloaded/ps5-homebrew-ui) by BlackBearReloaded, licensed GPL-3.0-or-later ([LICENSE](LICENSE)). Twispero draws its screens with it.

`app/projects/evoplayer/Makefile` compiles `src/gfx/draw_list.cpp`, `font.cpp` and `triangulate.cpp`, everything in `src/ui` and `src/core`, and `src/audio/cues.cpp`, `mixer.cpp` and `wav.cpp`. The draw lists they build go to the GPU through `app/projects/evoplayer/ui_kit/src/hui_agc_batch.cpp`.

The kit's demo app, design gallery, documentation, tests, tools and assets aren't part of this copy. They're in the upstream repository. [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) is upstream's notice for the whole kit.
