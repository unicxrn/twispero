#ifndef EVO_RMLUI_BRIDGE_H
#define EVO_RMLUI_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* How Twitch chat shows in the player (R1 changes it): the OSD's R1 hint. */
#define EVO_OSD_CHAT_SIDE    0   /* beside the video */
#define EVO_OSD_CHAT_OVERLAY 1   /* over it */
#define EVO_OSD_CHAT_OFF     2

/*
 * The player's OSD. A Twitch stream (`twitch`) shows its channel at the top
 * left (avatar, name, stream title, category and viewers), the quality
 * playing at the top right and the Twitch keys in the hints; a Twitch video
 * or clip (`twitch_video` too) the same top with a timeline under it.
 * Anything else shows `title` and `metadata`, and a timeline unless it is
 * live.
 */
typedef struct {
    const char* title;     /* what plays; for a Twitch stream "<name> · <title>" (the RmlUi OSD) */
    const char* metadata;  /* the line under it; not shown for a Twitch stream */
    /* "Buffering", "Waiting for the server": why the picture stands still,
     * shown with the spinner. NULL/"" while it plays. */
    const char* status;
    double position_sec;
    double duration_sec;
    double percentage;
    int paused;
    int scrub_active;
    double scrub_target;
    /* A seek (or the first frames) is still on its way: the centre shows a
     * LOADING spinner instead of a picture that looks frozen. */
    int loading;
    /* The Twitch quality playing ("1080p60"), shown at the top right; R2
     * opens the quality picker. NULL/"" when the stream has none to pick from. */
    const char* quality;
    /* A live stream: no timeline and no seek. */
    int is_live;
    int view_mode;   /* Triangle's aspect, for anything but a Twitch stream */
    int show_stats;
    /* A dev build (--usb-remote): Options shows the stats panel, and its hint
     * says so. 0 in a release build, which has neither. */
    int can_stats;
    int alpha;
    /* GL-5 (#81): dev FPS pill (#fps-pill), independent of the OSD chrome. */
    int fps;
    int debug_overlay;
    /* No picture (Twitch's "Audio only"): the OSD shows the channel in the
     * middle of the screen instead of over the video. */
    int music_mode;

    /* ---- A Twitch stream ---- */
    int twitch;
    const char* channel;        /* display name */
    const char* channel_login;  /* shown when the fonts can't draw the name */
    const char* stream_title;   /* NULL/"" when not known */
    const char* category;       /* NULL/"" when none or not known */
    int viewers;                /* -1 when not known */
    /* The channel's avatar as the image service hands it out (rows of R,G,B,A
     * bytes, as evo_hui_twitch_image_t): avatar_rgba is NULL until it has
     * arrived; avatar_id keys the texture. Not copied: it must stay valid
     * until this frame's evo_rmlui_render_playback_osd has returned. */
    const unsigned char* avatar_rgba;
    int avatar_w;
    int avatar_h;
    unsigned long long avatar_id;
    int chat_mode;              /* EVO_OSD_CHAT_*: R1's hint */
    int can_chat;               /* logged in: Triangle writes to chat, and its hint says so */
    int can_clip;               /* logged in: Square makes a clip, and its hint says so */

    /* ---- A Twitch past broadcast, highlight, upload or clip (with `twitch`) ----
     * The channel block as for a stream, with stream_title the video's title
     * and video_meta under it after the category; the timeline as for a
     * file; no chat keys. */
    int twitch_video;
    const char* video_meta;     /* "Past broadcast · 3 days ago", "Clip by Name · 12K views" */

    /* The clip card is up (with `twitch`): Circle closes it and the card's
     * own hint says so, so the OSD's Back hint steps aside. */
    int clip_card;
} evo_playback_osd_params_t;

/*
 * GL-5 (#81) / #63 first pass: the playback diagnostic HUD ("stats for nerds").
 * line_* are preformatted rows; the *_hist arrays are 0..1 sparkline samples
 * (oldest first, `hist_len` valid). Pushed only while the HUD is visible.
 */
typedef struct {
    const char* line_video;
    const char* line_audio;
    const char* line_perf;
    const char* line_queues;
    const char* line_clocks;
    int          hist_len;
    const float* gpu_hist;
    const float* ram_hist;
    const float* cpu_hist;
    float gpu_pct, gpu_peak_pct;
    float ram_mb,  ram_peak_mb, ram_total_mb;
    float cpu_pct, cpu_peak_pct;
} evo_perf_hud_t;

typedef struct {
    const char* eyebrow;
    const char* title;
    const char* detail;
    double progress_pct; // 0.0 to 1.0, or -1.0
    int action_count;
    int focused_action;  /* #65: index of the D-pad-focused button, -1 = none */
    struct {
        const char* icon_path;
        const char* label;
        int is_primary;
    } actions[3];
} evo_rmlui_dialog_params_t;

/* #75: toast notifications. kind: 0=info 1=tech 2=error 3=ok. alpha/slide
 * are the caller-owned fade/slide-in values evo_toast.c already computes
 * (0..255 and px-still-to-travel, matching the old evo_toast/evo_widget_toast
 * struct exactly) - the bridge just forwards them to CSS opacity/transform. */
typedef struct {
    const char* title;
    const char* message;
    int kind;
    int visible;
    int alpha;
    int slide;
    int bottom;   /* the kit's card: its lower edge on the 1080p canvas, 0 = just above the hint bars */
} evo_rmlui_toast_params_t;

/* How a settings row presents itself. macOS-style: booleans are switches you
 * can read at a glance, not ON/OFF text you have to parse. */
enum {
    EVO_RMLUI_ROW_ACTION = 0,  /* chevron only - opens a page or runs something */
    EVO_RMLUI_ROW_TOGGLE = 1,  /* switch, driven by toggle_on */
    EVO_RMLUI_ROW_VALUE  = 2,  /* collapsed: badge + chevron; expands to OPTIONs */
    EVO_RMLUI_ROW_OPTION = 3   /* one choice under an expanded VALUE row */
};

/* Display rows the settings document can show at once. A VALUE row expands
 * in place to list every choice, so the pool has to cover the longest list
 * (Default quality's nine) plus the section's own rows. */
#define EVO_RMLUI_SETTINGS_ROWS 20

/* The Settings sections, in sidebar order: Twitch, Controls & sound, System
 * (SettingsScreen.cpp's kSections). */
#define EVO_RMLUI_SETTINGS_SECTIONS 3

typedef struct {
    const char* title;
    const char* detail;
    const char* icon_path;
    const char* badge;
    int has_chevron;
    int is_focused;
    int kind;       /* EVO_RMLUI_ROW_* */
    int toggle_on;  /* TOGGLE: switch state. OPTION: is this the current choice. */
    int is_disabled; /* dimmed, no chevron; it depends on another setting */
} evo_rmlui_settings_row_t;

typedef struct {
    const char* title;
    const char* subtitle;
    const char* counter;
    int rail_active_idx;
    int rail_focused;
    /* Two-pane settings: which of the EVO_RMLUI_SETTINGS_SECTIONS sections
     * the sidebar highlights (-1 = none), and whether the sidebar or the
     * detail pane owns focus. The section labels are static, so they live in
     * settings.rml (and the kit's evo_hui_settings.cpp). */
    int section_active;
    int sidebar_focused;
    int row_count;
    evo_rmlui_settings_row_t rows[EVO_RMLUI_SETTINGS_ROWS];
} evo_rmlui_settings_params_t;

/*
 * Generic scrolling list screen: one document for every list (the quality
 * picker today).
 *
 * The caller hands over the visible window, already sliced: the DOM holds a
 * fixed number of rows and never grows, so a list of five thousand entries
 * costs the same as one of five.
 */
#define EVO_RMLUI_LIST_ROWS 9
#define EVO_RMLUI_LIST_MENU_ROWS 7

typedef struct {
    const char* title;
    const char* detail;
    const char* icon_path;
    const char* badge;
    int         progress;     /* 0..1000, or -1 for no bar */
    int         has_chevron;
    int         is_focused;
    /* The choice in effect (the quality playing): a tick at the row's end.
     * The RmlUi list has no tick and says PLAYING in the badge instead. */
    int         is_checked;
} evo_rmlui_list_row_t;

typedef struct {
    const char* title;
    const char* subtitle;
    int         section;      /* the rail section to light */
    int         rail_focused;

    int         row_count;    /* rows populated below (the visible window) */
    int         total_count;  /* full list length, for the "n OF m" counter */
    int         cursor_index; /* absolute index under the cursor, or -1 */
    evo_rmlui_list_row_t rows[EVO_RMLUI_LIST_ROWS];

    /* Empty state. A list with nothing in it says why, rather than
     * presenting an empty frame the user has to interpret. */
    int         is_empty;
    const char* empty_title;
    const char* empty_hint;
    const char* empty_icon;

    /* Footer hints vary per screen. */
    int         hint_count;
    struct {
        const char* glyph_path;
        const char* label;
    } hints[4];

    /* OPTIONS menu over the list. menu_count 0 = closed. */
    int         menu_count;
    int         menu_focus;
    const char* menu_eyebrow;
    const char* menu_title;
    const char* menu_sub;
    const char* menu_icon;
    struct {
        const char* label;
        const char* desc;
        const char* icon_path;
        int         danger;
    } menu[EVO_RMLUI_LIST_MENU_ROWS];
} evo_rmlui_list_params_t;


/* Initialize RmlUi Retained Engine */
bool evo_rmlui_init(int screen_width, int screen_height);
void evo_rmlui_shutdown(void);
bool evo_rmlui_is_initialized(void);

/* GL-3 (#79) B2: 1 if the UI changed since the last render and the device GL
 * frame loop should redraw + eglSwapBuffers this iteration; 0 to hold the front
 * buffer (retained-mode + ps5-opengl is too slow to re-raster every frame). */
int  evo_rmlui_needs_frame(void);
/* 1 if a Context::Render() actually composited to fb 0 since the last call
 * (clears the flag). The device loop swaps only when this is set. */
int  evo_rmlui_consume_drew(void);
/* Gate for this frame: 1 = the dispatch may render (a redraw frame), 0 = hold. */
void evo_rmlui_set_active(int active);
/* Called after a successful present: clears the UI-dirty flag. */
void evo_rmlui_end_frame(void);
/* 1 = CPU rasterise + GL blit (default), 0 = RmlUi renders itself through GL3
 * (the /mnt/usb0/evo_gl_rmlui path). Decides the main.c present step. */
int  evo_rmlui_blit_mode(void);

/* Playback OSD API */
void evo_rmlui_update_playback_params(const evo_playback_osd_params_t* params);
void evo_rmlui_update_perf_hud(const evo_perf_hud_t* hud);   /* #81/#63 */
void evo_rmlui_render_playback_osd(uint32_t* framebuffer, int width, int height);

/* Confirmation & Modal Dialog API */
void evo_rmlui_update_dialog(const evo_rmlui_dialog_params_t* params);
void evo_rmlui_render_dialog(uint32_t* framebuffer, int width, int height);

/* Toast notification API (#75). Renders in its own RmlUi context, composited
 * over whatever the caller already rendered into framebuffer this frame -
 * call evo_rmlui_render_toast() AFTER the screen's own render call. */
void evo_rmlui_update_toast(const evo_rmlui_toast_params_t* params);
void evo_rmlui_render_toast(uint32_t* framebuffer, int width, int height);

/* Dev debug overlay - the menu-screen FPS pill (GL-5 kept the FPS readout
 * player-only; this restores it for every non-player screen). Own RmlUi
 * context, composited like the toast: call update then render AFTER the
 * screen's own render call. `visible` follows show_debug_overlay. */
void evo_rmlui_update_debug_overlay(int fps, int visible);
void evo_rmlui_render_debug_overlay(uint32_t* framebuffer, int width, int height);

/* Virtual keyboard modal API (#81 / closes #34). evo_keyboard.c keeps the
 * buffer, layer and D-pad navigation state and pushes it here; this renders it
 * as an RmlUi document in its own context, composited over the screen like the
 * toast. */
typedef struct {
    int visible;
    int native_only;          /* native IME up: just dim, no panel */
    const char* title;
    const char* text;         /* current buffer (UTF-8) */
    const char* mode_label;   /* "LOWERCASE" / "UPPERCASE" / "SYMBOLS" */
    const char* rows[4];      /* 10 glyphs each, current layer */
    const char* action_labels[6];
    int len;
    int max_len;
    int focus_row;            /* 0..4 (4 = action bar) */
    int focus_col;
    int show_caret;
} evo_keyboard_params_t;
void evo_rmlui_update_keyboard(const evo_keyboard_params_t* params);
void evo_rmlui_render_keyboard(uint32_t* framebuffer, int width, int height);

/* Generic list screen API (the quality picker) */
void evo_rmlui_update_list(const evo_rmlui_list_params_t* params);
void evo_rmlui_render_list(uint32_t* framebuffer, int width, int height);

/* Settings API */
void evo_rmlui_update_settings(const evo_rmlui_settings_params_t* params);
void evo_rmlui_render_settings(uint32_t* framebuffer, int width, int height);

/* About & Support Screen API */
typedef struct {
    const char* app_name;        /* "EVO PLAYER PRO" */
    const char* version;         /* "v0.12.0" */
    const char* build_tag;       /* "PS5 HOMEBREW" */
    const char* tagline;         /* "CINEMATIC MEDIA PLAYER FOR PLAYSTATION 5 HOMEBREW" */
    const char* themes_info;     /* unused by the app: "" */
    int         action_focused;  /* 1 if "View Changelog" button is focused, 0 if rail is focused */
} evo_rmlui_about_params_t;

void evo_rmlui_update_about(const evo_rmlui_about_params_t* params);
void evo_rmlui_render_about(uint32_t* framebuffer, int width, int height);

/* Safe-to-close screen: the frame EVO parks on after QUIT (closed.rml). */
void evo_rmlui_render_closed(uint32_t* framebuffer, int width, int height);

/* A screen only the kit draws (ui_kit/, the Twitch screens): no RmlUi document
 * behind it. On a frame that redraws, calls render(width, height) and tells
 * RmlUi the kit drew it. Returns render's result; 0 when the frame holds or
 * the kit is off (the screen then shows nothing new). */
int evo_rmlui_render_kit_screen(int (*render)(int width, int height), int width, int height);

/* 1 once the kit tried to start and could not (evo_hui_start_failed). The
 * Twitch screens then draw evo_rmlui_render_twitch_fallback instead and take
 * its input: one RmlUi page that says the screens could not start, with two
 * buttons, Open Settings (focus 0) and Quit (focus 1). */
int  evo_rmlui_twitch_fallback_active(void);
void evo_rmlui_render_twitch_fallback(uint32_t* framebuffer, int width, int height, int focus);

/* Footer build number, shown on every screen. Set once at startup — it was
 * a literal in each .rml and had already drifted a release behind. */
void evo_rmlui_set_version(const char* version);

/* Centralized Theme API */
typedef struct {
    const char* name;
    uint32_t bg_top;
    uint32_t bg_bottom;
    uint32_t surface;
    uint32_t surface_sel;
    uint32_t border;
    uint32_t border_sel;
    uint32_t accent;
    uint32_t accent_soft;
    uint32_t accent_alt;
    uint32_t text_primary;
    uint32_t text_secondary;
    uint32_t text_muted;
} evo_rmlui_theme_t;

void evo_rmlui_set_theme(const evo_rmlui_theme_t* theme);

/* Sidebar navigation rail API */
typedef struct {
    int active_section;   /* ScreenManager's sections: 0 Home 1 Following 2 Browse 3 Search 4 Settings */
    int rail_focused;     /* 0=collapsed, 1=expanded */
    int cursor_index;     /* which item has cursor when rail is expanded */
    int visible;          /* 1=show rail, 0=hide (full-screen OSD, modals) */
    int fps;              /* render FPS for the rail pill */
    int show_fps;         /* 1 = DEBUG OVERLAY is on */
    int storage_locked;   /* 1 = per-title sandbox never lifted; show the
                           * elevated-privileges banner on every screen */
} evo_rmlui_nav_params_t;

void evo_rmlui_update_nav(const evo_rmlui_nav_params_t* params);

#ifdef __cplusplus
}
#endif

#endif /* EVO_RMLUI_BRIDGE_H */
