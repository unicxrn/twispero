/*
 * evo_prx_keep.c - keeps the libSceAudioIn and libSceWebBrowserDialog imports.
 *
 * scripts/package-app.sh links every PRX stub positionally, so both modules
 * stay in the app module's NEEDED list. Stage 6 removed their last callers
 * (the speaker calibration's microphone and the web UI), and the module
 * writer (tools/native-app/sce_module_writer.cpp) builds the import table
 * only from symbols the link actually uses: the two modules were left NEEDED
 * with no import at all, a table the console has never loaded. The stage 5
 * build that booted on 12.70 imported exactly the ten functions below, and
 * one call to each here brings that table back.
 *
 * None of the calls ever runs. main() calls evo_prx_keep_imports() once, and
 * it returns at once because evo_prx_keep_never is 0. The flag is volatile so
 * the compiler can't see that and drop the calls, and the imports with them.
 * The dead-import check in package-app.sh fails the build if a name in
 * libSceAudioIn.syms or libSceWebBrowserDialog.syms is no longer referenced.
 */
#ifdef EVO_APP_MODULE

#include "evo_prx_keep.h"

#include <stddef.h>
#include <stdint.h>

/* As the stage 5 callers declared them (SpeakerCalibrationService.cpp,
 * evo_webui.c); the two dialog structs are only ever passed as NULL here. */
int32_t sceAudioInOpen(int32_t user_id, int32_t type, int32_t index,
                       uint32_t grain, uint32_t frequency, uint32_t format);
int32_t sceAudioInInput(int32_t handle, void *destination);
int32_t sceAudioInGetSilentState(int32_t handle);
int32_t sceAudioInClose(int32_t handle);

int sceWebBrowserDialogInitialize(void);
int sceWebBrowserDialogOpen(void *param);
int sceWebBrowserDialogUpdateStatus(void);
int sceWebBrowserDialogGetResult(void *result);
int sceWebBrowserDialogClose(void);
int sceWebBrowserDialogTerminate(void);

volatile int evo_prx_keep_never = 0;

void evo_prx_keep_imports(void)
{
    if (!evo_prx_keep_never)
        return;

    const int32_t mic = sceAudioInOpen(0, 0, 0, 0, 0, 0);
    (void)sceAudioInInput(mic, NULL);
    (void)sceAudioInGetSilentState(mic);
    (void)sceAudioInClose(mic);

    (void)sceWebBrowserDialogInitialize();
    (void)sceWebBrowserDialogOpen(NULL);
    (void)sceWebBrowserDialogUpdateStatus();
    (void)sceWebBrowserDialogGetResult(NULL);
    (void)sceWebBrowserDialogClose();
    (void)sceWebBrowserDialogTerminate();
}

#endif /* EVO_APP_MODULE */
