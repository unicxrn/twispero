/*
 * evo_prx_keep.h - see src/evo_prx_keep.c. App module only.
 */
#ifndef EVO_PRX_KEEP_H
#define EVO_PRX_KEEP_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef EVO_APP_MODULE
/* Called once from main(). Does nothing: its never-run calls keep the
 * libSceAudioIn and libSceWebBrowserDialog imports the console has loaded. */
void evo_prx_keep_imports(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* EVO_PRX_KEEP_H */
