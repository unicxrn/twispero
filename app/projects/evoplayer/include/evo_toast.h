#ifndef EVO_TOAST_MODULE_H
#define EVO_TOAST_MODULE_H

/*
 * Module: evo_toast
 *
 * In-app toast notifications. Each toast says what kind it is, which sets its
 * colour; nothing looks at the words (a stream title with "fail" in it is
 * not an error, and one that says "selected" still shows). Developer detail
 * (EVO_TOAST_TECH) only shows with the debug overlay or the debug profile on.
 *
 * Public surface:
 *   toast_kind(kind, title, msg)   - fire-and-forget notification
 *   toast / toast_error / toast_success / toast_tech - shorthands
 *   draw_prospero_toast(fb)        - called every frame by the main render loop
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The kinds, as evo_rmlui_toast_params_t.kind carries them to the screen. */
enum {
    EVO_TOAST_INFO    = 0,  /* the accent colour: progress, a note, a setting changed */
    EVO_TOAST_TECH    = 1,  /* developer detail: hidden unless debugging */
    EVO_TOAST_ERROR   = 2,  /* red: something didn't work */
    EVO_TOAST_SUCCESS = 3   /* green: something finished or came back */
};

/* Fire a toast of this kind (an unknown kind is info). title and msg may be
 * NULL or "": a toast with only one of them is a single line, and one with
 * neither isn't shown. */
void toast_kind(int kind, const char *title, const char *msg);

/* An info toast. The callers from before the kinds keep working with it. */
void toast(const char *title, const char *msg);

static inline void toast_error(const char *title, const char *msg) { toast_kind(EVO_TOAST_ERROR, title, msg); }
static inline void toast_success(const char *title, const char *msg) { toast_kind(EVO_TOAST_SUCCESS, title, msg); }
static inline void toast_tech(const char *title, const char *msg) { toast_kind(EVO_TOAST_TECH, title, msg); }

/* Where the toast's lower edge goes on the 1080p canvas from now on: 0 for
 * the default (just above the pages' hint bars), or a smaller y to keep it
 * above a screen's own controls (the player's timeline). The kit glides it
 * there. */
void toast_set_bottom(int y);

/* Draw the current toast into the framebuffer. Call every frame. */
void draw_prospero_toast(uint32_t *fb);

/* 1 while a toast is on screen (including its fade-out). The GL video path uses
 * this to know the OSD scratch has content worth compositing over the frame. */
int evo_toast_visible(void);

#ifdef __cplusplus
}
#endif

#endif /* EVO_TOAST_MODULE_H */
