/*
 * evo_hui.h - EVO's ps5-homebrew-ui screens, as seen by the RmlUi bridge.
 *
 * The bridge (ui_rml/src/evo_rmlui_bridge.cpp) hands every update to these
 * first. A render call returns 1 when the kit drew the screen into the
 * current AGC frame, 0 when the caller should fall back to the RmlUi screen
 * (kit not built, ui_sdf.pipe missing, fonts failed, or the screen is not
 * ported yet). Main thread only.
 *
 * EVO's kill switch (/mnt/usb0/evo_no_hui keeps every screen on RmlUi) is
 * ignored in ps5-twitch: the Twitch screens have no RmlUi version.
 */
#ifndef EVO_HUI_H
#define EVO_HUI_H

#include "evo_rmlui_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

void evo_hui_shutdown(void);

/* 1 once the kit tried to start and could not (no GPU runtime, the draw batch
 * or a font failed); the reason is in evo.log. The screens only the kit draws
 * (the Twitch ones) then show one RmlUi page instead (twitch_fallback.rml,
 * evo_rmlui_render_twitch_fallback). Not set by evo_hui_shutdown(). */
int  evo_hui_start_failed(void);

/* 1 while a kit screen is up and animating: the device loop must keep
 * drawing frames. */
int  evo_hui_wants_frame(void);
/* Once per presented frame. */
void evo_hui_end_frame(void);

void evo_hui_set_theme(const evo_rmlui_theme_t* theme);
void evo_hui_update_nav(const evo_rmlui_nav_params_t* params);

/* Dev remote (#115): the kit screen on display and its focused item, or NULL
 * when no kit screen drew last frame. `*screen_doc` is the screen's name
 * ("hui_twitch_home"); the return value is one JSON object in the shape
 * evo_rmlui_devstate.cpp writes for an RmlUi element. Static storage. */
const char* evo_hui_dev_focus_json(const char** screen_doc);

void evo_hui_update_list(const evo_rmlui_list_params_t* params);
int  evo_hui_render_list(int width, int height);

void evo_hui_update_settings(const evo_rmlui_settings_params_t* params);
int  evo_hui_render_settings(int width, int height);

/* Player OSD. */
void evo_hui_update_playback(const evo_playback_osd_params_t* params);
void evo_hui_update_perf_hud(const evo_perf_hud_t* hud);
int  evo_hui_render_playback_osd(int width, int height);

/* Where the OSD goes, as fractions of the screen; (0, 0, 1, 1), the default,
 * is the whole screen. The OSD's 1920 x 1080 canvas is fitted into the area,
 * keeping its aspect, centred. Values are clamped to [0, 1], and an area with
 * w or h below 0.05 is the whole screen. Only the OSD: toasts, dialogs and
 * the other screens stay full screen. Set it every frame the OSD draws (the
 * player does, to the video's box while chat is beside it). */
void evo_hui_set_osd_area(float x, float y, float w, float h);

/* The fit evo_hui_set_osd_area makes, in canvas units: a canvas point p
 * lands at p * scale + (dx, dy). Also for the host previews. */
static inline void evo_hui_osd_area_fit(float x, float y, float w, float h,
                                        float* scale, float* dx, float* dy)
{
    const float cw = 1920.0f, ch = 1080.0f;
    float r[4];
    int i;
    r[0] = x; r[1] = y; r[2] = w; r[3] = h;
    for (i = 0; i < 4; ++i) {
        if (!(r[i] >= 0.0f)) r[i] = 0.0f; /* NaN too */
        if (r[i] > 1.0f) r[i] = 1.0f;
    }
    if (r[2] < 0.05f || r[3] < 0.05f) {
        r[0] = 0.0f; r[1] = 0.0f; r[2] = 1.0f; r[3] = 1.0f;
    }
    {
        const float bw = r[2] * cw, bh = r[3] * ch;
        const float s = (bw / cw < bh / ch) ? bw / cw : bh / ch;
        *scale = s;
        *dx = r[0] * cw + (bw - cw * s) * 0.5f;
        *dy = r[1] * ch + (bh - ch * s) * 0.5f;
    }
}


void evo_hui_update_about(const evo_rmlui_about_params_t* params);
int  evo_hui_render_about(int width, int height);
int  evo_hui_render_closed(int width, int height);

/* Overlays, drawn over whatever screen this frame already has. */
void evo_hui_update_dialog(const evo_rmlui_dialog_params_t* params);
int  evo_hui_render_dialog(int width, int height);
void evo_hui_update_toast(const evo_rmlui_toast_params_t* params);
int  evo_hui_render_toast(int width, int height);

/* The virtual keyboard: an overlay that draws only while it is visible. */
void evo_hui_update_keyboard(const evo_keyboard_params_t* params);
int  evo_hui_render_keyboard(int width, int height);

#ifdef __cplusplus
}
#endif

#endif /* EVO_HUI_H */
