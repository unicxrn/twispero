/*
 * Module: evo_toast
 *
 * State and renderer for in-app toast notifications. The caller says which
 * kind a toast is (evo_toast.h).
 * Extracted from main.c (PROSPERO_TOAST_STATE_START / _END and
 * PROSPERO_TOAST_RENDERER_START / _END).
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "evo_toast.h"

/* Shared player globals this module needs read access to. */
extern int show_debug_overlay;

/* PlaybackProfile — mirrored here to avoid a circular include with main.c.
 * Only PROFILE_DEBUG is tested; the numeric value must match the enum. */
#define _PROFILE_DEBUG 3
extern int current_profile;

/* now_ms() — declared in main.c, linked into the same binary. */
extern long long now_ms(void);

/* #75: RmlUi bridge — replaces the CPU-rasterised evo_widget_toast(). */
#include "evo_rmlui_bridge.h"
#include "evo_metrics.h"   /* EVO_SCREEN_W/H — the toast document's fixed canvas */

/* --------------------------------------------------------------------------
 * State
 * -------------------------------------------------------------------------- */

static char      prospero_toast_title[96]   = {0};
static char      prospero_toast_message[256] = {0};
static long long prospero_toast_started_ms  = 0;
static int       prospero_toast_active      = 0;
static int       prospero_toast_kind        = 0;  /* EVO_TOAST_* */
static int       prospero_toast_bottom      = 0;  /* toast_set_bottom(); 0 = the default */

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

void toast_kind(int kind, const char *title, const char *msg)
{
    if (!title) title = "";
    if (!msg)   msg   = "";
    if (!title[0] && !msg[0])
        return;
    if (kind < EVO_TOAST_INFO || kind > EVO_TOAST_SUCCESS)
        kind = EVO_TOAST_INFO;

    /*
     * Normal users do not need decoder initialisation, conversion-path or
     * fallback details. The kind is the caller's: the words are never read
     * (they used to be, and a stream title with "fail" in it showed red).
     */
    if (kind == EVO_TOAST_TECH &&
        !(show_debug_overlay || current_profile == _PROFILE_DEBUG))
        return;

    snprintf(prospero_toast_title,   sizeof(prospero_toast_title),   "%s", title);
    snprintf(prospero_toast_message, sizeof(prospero_toast_message), "%s", msg);

    prospero_toast_started_ms = now_ms();
    prospero_toast_active     = 1;
    prospero_toast_kind       = kind;

    /*
     * EVO: the console's own notification banner is deliberately not used.
     *
     * This used to also fire sceNotificationSend() for technical messages,
     * which put a PlayStation system toast over the app — the OS chrome, the
     * OS position, the OS timing, none of it themeable and none of it
     * dismissable by us. The player has its own toast, it is themed, and it
     * is drawn inside our own frame; there is no reason for a second
     * notification system that we do not control.
     */
}

void toast(const char *title, const char *msg)
{
    toast_kind(EVO_TOAST_INFO, title, msg);
}

void toast_set_bottom(int y)
{
    prospero_toast_bottom = y > 0 ? y : 0;
}

void draw_prospero_toast(uint32_t *fb)
{
    /*
     * #75: timing and state still live here; every pixel is now
     * evo_rmlui_render_toast()'s (its own RmlUi context — see the comment on
     * EvoToastState in evo_rmlui_app.h). The RmlUi document IS the theme —
     * SetTheme-driven colours instead of the CPU renderer's baked literals,
     * which used to stay cyan under CARBON, EMBER and AURORA.
     *
     * evo_rmlui_update_toast() is called even while inactive, with
     * visible=0, so a toast that just expired hides its RmlUi document
     * immediately rather than leaving the last frame's card on screen.
     */
    const int hold_ms  = 2200;
    const int fade_ms  = 360;
    const int slide_ms = 180;
    const int slide_px = 90;

    long long elapsed;
    evo_rmlui_toast_params_t p;

    memset(&p, 0, sizeof p);

    if (!prospero_toast_active) {
        evo_rmlui_update_toast(&p);
        return;
    }

    elapsed = now_ms() - prospero_toast_started_ms;

    if (elapsed >= hold_ms + fade_ms) {
        prospero_toast_active = 0;
        evo_rmlui_update_toast(&p);
        return;
    }

    p.title   = prospero_toast_title;
    p.message = prospero_toast_message;
    p.kind    = prospero_toast_kind;   /* EVO_TOAST_*: evo_rmlui_toast_params_t's kinds */
    p.bottom  = prospero_toast_bottom;
    p.visible = 1;

    p.alpha = (elapsed > hold_ms)
        ? 255 - (int)((elapsed - hold_ms) * 255 / fade_ms)
        : 255;
    if (p.alpha < 0) p.alpha = 0;

    p.slide = (elapsed < slide_ms)
        ? slide_px - (int)(elapsed * slide_px / slide_ms)
        : 0;

    evo_rmlui_update_toast(&p);
    evo_rmlui_render_toast(fb, EVO_SCREEN_W, EVO_SCREEN_H);
}

int evo_toast_visible(void)
{
    /* Cleared lazily by draw_prospero_toast() once hold+fade elapses; the
     * render loop calls that before it consults this, so the answer is
     * current for the frame being composited. */
    return prospero_toast_active;
}
