# PRX import stubs

The SDK ships link stubs (`$PS5_SYSROOT/lib/libSce*.so`) for ~31 system
modules. It has none for the ones EVO's native-decode / GPU work needs, and a
fake-signed app module **cannot** `sceKernelLoadStartModule` a system PRX it did
not declare `NEEDED` (proven on hardware 2026-09-02/03: both `libSceAgc.sprx`
and `libSceAvPlayer.sprx` load-failed).

Each `*.syms` here is a plain list of exported symbol names (one per line,
`#` comments allowed). `scripts/package-app.sh` turns each into a tiny ELF
`.so` (SONAME `<name>.sprx`, one empty `FUNC` per symbol) and:

1. adds it to the intermediate link positionally, not `--as-needed`, like
   ProsperoLight: every stub in the list is a `NEEDED` entry of the app
   module whether EVO references it or not, and
2. passes it to `native_app_builder link --stub …`, which matches each
   undefined symbol → this stub → computes the Sony NID from the plain name →
   emits a proper PS5 import. The loader then auto-loads the `.sprx` at start.

Only the names EVO actually references become imports: lld leaves an unused
stub symbol out of `.dynsym`, and the module writer builds the import table
from `.dynsym`. A name nothing references is simply not imported, so
`package-app.sh` refuses to build with one (its dead-import check) unless the
line carries `# keep: <reason>`.

So: to use one of these modules, just `extern`-declare and call the function.
No `sceKernelLoadStartModule`, no `sceKernelDlsym`, no NID maths.

**Adding a symbol:** append its name to the right `.syms` file. If a link
fails with `no public SDK stub exports required symbol sceXxx`, that is the
missing name — add it.

**A module nothing uses any more** still stays `NEEDED`, so it has to keep the
imports the console has already loaded. `libSceAudioIn` and
`libSceWebBrowserDialog` lost their callers in stage 6 (the microphone and the
web UI); `projects/evoplayer/src/evo_prx_keep.c` calls each of their functions
in code that never runs, which keeps the exact import table the stage 5 build
booted with on 12.70.

Symbol names verified against `third_party/SharpProspero` (`[LibraryImport(...)]`
attributes) and the EVO headers in `projects/evoplayer/media/include/sce/`.

## /system/priv/lib is out of reach (tested 2026-09-21)

Every module listed here is from `/system/common/lib`. A privileged one is
**not** loadable by a fake-signed app module, and the failure is total:

- Tried: `libSceVcodec.sprx` (`/system/priv/lib`, module 0x80000091) for
  `sceVcodecGetHwBudgetSize` (NID `Ipf4PM5PIHE`, from
  `third_party/new_nid.txt`). The builder derived that exact NID from the
  plain name, so the symbol name was right.
- Result: the app did not start. No `evo.log`, no `evo_status`, nothing on
  screen - the loader rejected the module over the unbindable `NEEDED`
  import, i.e. the CE-108255-1 case described above.

This forecloses the whole directory, not just that one symbol. `/system/priv/lib`
also holds `libSceAudiodecCpuOpus`, `libSceAudiodecCpuDtsHdMa`,
`libSceAudiodecCpuTrhd` and `libSceAudiodecCpuLpcm` - hardware decoders for
audio EVO currently handles in FFmpeg. They are unreachable by this route too.

Anything needed from a privileged module has to come from somewhere else
(a different exporting module in `common/lib`, or the daemon-side jailbreak),
not from adding a `.syms` here.
