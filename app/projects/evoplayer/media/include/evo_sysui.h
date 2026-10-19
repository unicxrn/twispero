#ifndef EVO_SYSUI_H
#define EVO_SYSUI_H

/*
 * evo_sysui - has the PS5 moved its focus away from the app (to its home
 * screen, or to close the app)?
 *
 * Closing an app from the PS button's menu kills it outright. A kill while
 * the app still had GPU work in flight panicked the console once
 * (2026-09-18). On hardware (2026-10-19) the system moved the focus to
 * itself 1-2 s before every such kill, so the frame loop stops drawing and
 * drains the GPU the moment the focus leaves (Application.cpp), and the kill
 * then finds the same quiet app the soft close leaves. The PS button's quick
 * menu over a running app does not move the focus; nothing stops for it.
 *
 * The focus is ShellCore's "SceShellCoreUtilAppFocus" kernel event flag (its
 * low 32 bits name the focused app, as ShadowMount+ reads it), compared with
 * the app's own id from sceKernelGetAppInfo(). On 13.60 the system's own
 * screen reads 0x7.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Opens the flag and reads the app's id, once, before the self-unjail (with
 * evo_hw_probe()). What fails is logged, and the open is retried later. */
void evo_sysui_probe(void);

/* 1 while another app (the system) has the focus, 0 while this app has it,
 * -1 when it can't tell (then the app carries on as before). One syscall a
 * call; each change is logged. Main thread. */
int evo_sysui_focus_lost(void);

#ifdef __cplusplus
}
#endif

#endif /* EVO_SYSUI_H */
