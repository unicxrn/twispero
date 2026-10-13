#include "evo_rmlui_bridge.h"
#include "evo_rmlui_app.h"
#include "evo_rmlui_devstate.h"
#if defined(EVO_AGC_DEVICE)
#include "evo_hui.h"
#endif

#include <cstdio>
#include <string>

namespace {

/* A viewer count the short way, as the kit's format_viewers() writes it:
 * 999, 1.2K, 12.4K, 120K, 1.1M (cut, never rounded up). */
std::string compact_count(int count) {
    const long long v = count < 0 ? 0 : count;
    char b[24];
    if (v < 1000)
        std::snprintf(b, sizeof b, "%lld", v);
    else if (v >= 100000 && v < 1000000)
        std::snprintf(b, sizeof b, "%lldK", v / 1000);
    else {
        const long long unit = v < 1000000 ? 1000 : 1000000;
        const long long tenths = v * 10 / unit;
        const char* suffix = unit == 1000 ? "K" : "M";
        if (tenths % 10 == 0)
            std::snprintf(b, sizeof b, "%lld%s", tenths / 10, suffix);
        else
            std::snprintf(b, sizeof b, "%lld.%lld%s", tenths / 10, tenths % 10, suffix);
    }
    return b;
}

}  // namespace

extern "C" {

bool evo_rmlui_init(int screen_width, int screen_height) {
    return EvoRmlApp::Instance().Initialize(screen_width, screen_height);
}

void evo_rmlui_shutdown(void) {
#if defined(EVO_AGC_DEVICE)
    evo_hui_shutdown();
#endif
    EvoRmlApp::Instance().Shutdown();
}

bool evo_rmlui_is_initialized(void) {
    return EvoRmlApp::Instance().IsInitialized();
}

int evo_rmlui_needs_frame(void) {
    return EvoRmlApp::Instance().GlNeedsFrame() ? 1 : 0;
}

int evo_rmlui_consume_drew(void) {
    return EvoRmlApp::Instance().GlConsumeDrew() ? 1 : 0;
}

void evo_rmlui_set_active(int active) {
    EvoRmlApp::Instance().GlSetActive(active != 0);
}

void evo_rmlui_end_frame(void) {
#if defined(EVO_AGC_DEVICE)
    evo_hui_end_frame();
#endif
    EvoRmlApp::Instance().GlEndFrame();
}

int evo_rmlui_blit_mode(void) {
    return EvoRmlApp::Instance().GlBlitMode() ? 1 : 0;
}

void evo_rmlui_set_version(const char* version) {
    EvoRmlApp::Instance().SetVersion(version ? version : "");
}

void evo_rmlui_update_list(const evo_rmlui_list_params_t* p) {
    if (!p) return;
#if defined(EVO_AGC_DEVICE)
    evo_hui_update_list(p);
#endif
    EvoListState state;
    state.title = p->title ? p->title : "";
    state.subtitle = p->subtitle ? p->subtitle : "";
    state.section = p->section;
    state.rail_focused = (p->rail_focused != 0);
    state.total_count = p->total_count;
    state.cursor_index = p->cursor_index;

    for (int i = 0; i < p->row_count && i < EVO_RMLUI_LIST_ROWS; i++) {
        EvoListRow r;
        r.title = p->rows[i].title ? p->rows[i].title : "";
        r.detail = p->rows[i].detail ? p->rows[i].detail : "";
        r.icon_path = p->rows[i].icon_path ? p->rows[i].icon_path : "";
        r.badge = p->rows[i].badge ? p->rows[i].badge : "";
        if (p->rows[i].is_checked && r.badge.empty()) r.badge = "PLAYING";   /* no tick here */
        r.progress = p->rows[i].progress;
        r.has_chevron = (p->rows[i].has_chevron != 0);
        r.is_focused = (p->rows[i].is_focused != 0);
        state.rows.push_back(r);
    }

    state.is_empty = (p->is_empty != 0);
    state.empty_title = p->empty_title ? p->empty_title : "";
    state.empty_hint = p->empty_hint ? p->empty_hint : "";
    state.empty_icon = p->empty_icon ? p->empty_icon : "";

    for (int i = 0; i < p->hint_count && i < 4; i++) {
        EvoListHint h;
        h.glyph_path = p->hints[i].glyph_path ? p->hints[i].glyph_path : "";
        h.label = p->hints[i].label ? p->hints[i].label : "";
        state.hints.push_back(h);
    }

    for (int i = 0; i < p->menu_count && i < EVO_RMLUI_LIST_MENU_ROWS; i++) {
        EvoListMenuRow m;
        m.label = p->menu[i].label ? p->menu[i].label : "";
        m.desc = p->menu[i].desc ? p->menu[i].desc : "";
        m.icon_path = p->menu[i].icon_path ? p->menu[i].icon_path : "";
        m.danger = (p->menu[i].danger != 0);
        state.menu.push_back(m);
    }
    state.menu_focus = p->menu_focus;
    state.menu_eyebrow = p->menu_eyebrow ? p->menu_eyebrow : "";
    state.menu_title = p->menu_title ? p->menu_title : "";
    state.menu_sub = p->menu_sub ? p->menu_sub : "";
    state.menu_icon = p->menu_icon ? p->menu_icon : "";

    EvoRmlApp::Instance().UpdateListState(state);
}

void evo_rmlui_render_list(uint32_t* framebuffer, int width, int height) {
#if defined(EVO_AGC_DEVICE)
    if (EvoRmlApp::Instance().GlActive() && evo_hui_render_list(width, height)) {
        EvoRmlApp::Instance().NoteExternalDraw();
        return;
    }
#endif
    EvoRmlApp::Instance().RenderList(framebuffer, width, height);
}

void evo_rmlui_update_playback_params(const evo_playback_osd_params_t* p) {
    if (!p) return;
#if defined(EVO_AGC_DEVICE)
    evo_hui_update_playback(p);
#endif
    EvoPlaybackState state;
    state.title = p->title ? p->title : "";
    state.meta = p->metadata ? p->metadata : "";
    if (p->twitch) {
        /* "Category · 12.3K watching", as the kit's OSD says it; a video's
         * "Category · Past broadcast · 3 days ago". */
        state.meta = p->category ? p->category : "";
        if (p->twitch_video && p->video_meta && p->video_meta[0]) {
            if (!state.meta.empty()) state.meta += " \xC2\xB7 ";
            state.meta += p->video_meta;
        } else if (p->viewers >= 0) {
            if (!state.meta.empty()) state.meta += " \xC2\xB7 ";
            state.meta += compact_count(p->viewers) + " watching";
        }
    }
    state.status = p->status ? p->status : "";
    for (char& c : state.status)   /* the document's labels are capitals */
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    state.position_sec = p->position_sec;
    state.duration_sec = p->duration_sec;
    state.percentage = p->percentage;
    state.paused = (p->paused != 0);
    state.scrub_active = (p->scrub_active != 0);
    state.loading = (p->loading != 0);
    state.scrub_target = p->scrub_target;
    state.quality = p->quality ? p->quality : "";
    state.is_live = (p->is_live != 0);
    state.view_mode = p->view_mode;
    state.show_stats = (p->show_stats != 0);
    state.can_stats = (p->can_stats != 0);
    state.alpha = p->alpha;
    state.fps = p->fps;
    state.debug_overlay = (p->debug_overlay != 0);
    state.music_mode = (p->music_mode != 0);
    /* A video or clip has a file's keys and timeline: only a live stream
     * has the Twitch keys. */
    state.twitch = (p->twitch != 0 && p->twitch_video == 0);
    state.twitch_video = (p->twitch != 0 && p->twitch_video != 0);
    state.chat_mode = p->chat_mode;
    state.can_chat = (p->can_chat != 0);
    state.can_clip = (p->can_clip != 0);

    EvoRmlApp::Instance().UpdatePlaybackState(state);
#if defined(EVO_USB_REMOTE) && defined(EVO_APP_MODULE)
    evo_rmlui_dev_note_osd(state.alpha > 0,
                           state.show_stats, state.scrub_active);   /* #115 */
#endif
}

void evo_rmlui_update_perf_hud(const evo_perf_hud_t* p) {
#if defined(EVO_AGC_DEVICE)
    if (p) evo_hui_update_perf_hud(p);
#endif
    if (p) EvoRmlApp::Instance().UpdatePerfHud(p);
}

void evo_rmlui_update_keyboard(const evo_keyboard_params_t* p) {
#if defined(EVO_AGC_DEVICE)
    if (p) evo_hui_update_keyboard(p);
#endif
    if (p) EvoRmlApp::Instance().UpdateKeyboard(p);
}

void evo_rmlui_render_keyboard(uint32_t* framebuffer, int width, int height) {
#if defined(EVO_AGC_DEVICE)
    if (EvoRmlApp::Instance().GlActive() && evo_hui_render_keyboard(width, height)) {
        EvoRmlApp::Instance().NoteExternalKeyboard();
        return;
    }
#endif
    EvoRmlApp::Instance().RenderKeyboard(framebuffer, width, height);
}

void evo_rmlui_render_playback_osd(uint32_t* framebuffer, int width, int height) {
#if defined(EVO_AGC_DEVICE)
    if (EvoRmlApp::Instance().GlActive() && evo_hui_render_playback_osd(width, height)) {
        EvoRmlApp::Instance().NoteExternalDraw();
        return;
    }
#endif
    EvoRmlApp::Instance().RenderPlaybackOSD(framebuffer, width, height);
}

void evo_rmlui_update_dialog(const evo_rmlui_dialog_params_t* p) {
    if (!p) return;
#if defined(EVO_AGC_DEVICE)
    evo_hui_update_dialog(p);
#endif
    EvoDialogState state;
    state.eyebrow = p->eyebrow ? p->eyebrow : "";
    state.title = p->title ? p->title : "";
    state.detail = p->detail ? p->detail : "";
    state.progress_pct = p->progress_pct;
    state.focused_action = p->focused_action;

    for (int i = 0; i < p->action_count && i < 3; i++) {
        EvoDialogAction act;
        act.icon_path = p->actions[i].icon_path ? p->actions[i].icon_path : "../icons/btn_cross.png";
        act.label = p->actions[i].label ? p->actions[i].label : "";
        act.is_primary = (p->actions[i].is_primary != 0);
        state.actions.push_back(act);
    }

    EvoRmlApp::Instance().UpdateDialogState(state);
}

void evo_rmlui_render_dialog(uint32_t* framebuffer, int width, int height) {
#if defined(EVO_AGC_DEVICE)
    if (EvoRmlApp::Instance().GlActive() && evo_hui_render_dialog(width, height)) {
        EvoRmlApp::Instance().NoteExternalDialog();
        return;
    }
#endif
    EvoRmlApp::Instance().RenderDialog(framebuffer, width, height);
}

void evo_rmlui_update_toast(const evo_rmlui_toast_params_t* p) {
    if (!p) return;
#if defined(EVO_AGC_DEVICE)
    evo_hui_update_toast(p);
#endif
    EvoToastState state;
    state.title = p->title ? p->title : "";
    state.message = p->message ? p->message : "";
    state.kind = p->kind;
    state.visible = (p->visible != 0);
    state.alpha = p->alpha;
    state.slide = p->slide;

    EvoRmlApp::Instance().UpdateToastState(state);
}

void evo_rmlui_render_toast(uint32_t* framebuffer, int width, int height) {
#if defined(EVO_AGC_DEVICE)
    if (EvoRmlApp::Instance().GlActive() && evo_hui_render_toast(width, height)) {
        EvoRmlApp::Instance().NoteExternalToast();
        return;
    }
#endif
    EvoRmlApp::Instance().RenderToast(framebuffer, width, height);
}

void evo_rmlui_update_debug_overlay(int fps, int visible) {
    EvoRmlApp::Instance().UpdateDebugOverlay(fps, visible != 0);
}

void evo_rmlui_render_debug_overlay(uint32_t* framebuffer, int width, int height) {
    EvoRmlApp::Instance().RenderDebugOverlay(framebuffer, width, height);
}

void evo_rmlui_update_settings(const evo_rmlui_settings_params_t* p) {
    if (!p) return;
#if defined(EVO_AGC_DEVICE)
    evo_hui_update_settings(p);
#endif
    EvoSettingsState state;
    state.title = p->title ? p->title : "SETTINGS";
    state.subtitle = p->subtitle ? p->subtitle : "";
    state.counter = p->counter ? p->counter : "";
    state.rail_active_idx = p->rail_active_idx;
    state.rail_focused = (p->rail_focused != 0);
    state.section_active = p->section_active;
    state.sidebar_focused = (p->sidebar_focused != 0);

    for (int i = 0; i < p->row_count && i < EVO_RMLUI_SETTINGS_ROWS; i++) {
        EvoSettingsRow row;
        row.title = p->rows[i].title ? p->rows[i].title : "";
        row.detail = p->rows[i].detail ? p->rows[i].detail : "";
        row.icon_path = p->rows[i].icon_path ? p->rows[i].icon_path : "../icons/icon_settings.png";
        row.badge = p->rows[i].badge ? p->rows[i].badge : "";
        row.has_chevron = (p->rows[i].has_chevron != 0);
        row.is_focused = (p->rows[i].is_focused != 0);
        row.kind = p->rows[i].kind;
        row.toggle_on = (p->rows[i].toggle_on != 0);
        row.is_disabled = (p->rows[i].is_disabled != 0);
        state.rows.push_back(row);
    }

    EvoRmlApp::Instance().UpdateSettingsState(state);
}

void evo_rmlui_render_settings(uint32_t* framebuffer, int width, int height) {
#if defined(EVO_AGC_DEVICE)
    if (EvoRmlApp::Instance().GlActive() && evo_hui_render_settings(width, height)) {
        EvoRmlApp::Instance().NoteExternalDraw();
        return;
    }
#endif
    EvoRmlApp::Instance().RenderSettings(framebuffer, width, height);
}

void evo_rmlui_update_about(const evo_rmlui_about_params_t* p) {
    if (!p) return;
#if defined(EVO_AGC_DEVICE)
    evo_hui_update_about(p);
#endif
    EvoAboutState state;
    state.app_name = p->app_name ? p->app_name : "EVO PLAYER PRO";
    state.version = p->version ? p->version : "";
    state.build_tag = p->build_tag ? p->build_tag : "PS5 HOMEBREW";
    state.tagline = p->tagline ? p->tagline : "";
    state.themes_info = p->themes_info ? p->themes_info : "";
    state.action_focused = (p->action_focused != 0);

    EvoRmlApp::Instance().UpdateAboutState(state);
}

void evo_rmlui_render_about(uint32_t* framebuffer, int width, int height) {
#if defined(EVO_AGC_DEVICE)
    if (EvoRmlApp::Instance().GlActive() && evo_hui_render_about(width, height)) {
        EvoRmlApp::Instance().NoteExternalDraw();
        return;
    }
#endif
    EvoRmlApp::Instance().RenderAbout(framebuffer, width, height);
}

void evo_rmlui_render_closed(uint32_t* framebuffer, int width, int height) {
#if defined(EVO_AGC_DEVICE)
    if (EvoRmlApp::Instance().GlActive() && evo_hui_render_closed(width, height)) {
        EvoRmlApp::Instance().NoteExternalDraw();
        return;
    }
#endif
    EvoRmlApp::Instance().RenderClosed(framebuffer, width, height);
}

int evo_rmlui_render_kit_screen(int (*render)(int width, int height), int width, int height) {
#if defined(EVO_AGC_DEVICE)
    if (render && EvoRmlApp::Instance().GlActive() && render(width, height)) {
        EvoRmlApp::Instance().NoteExternalDraw();
        return 1;
    }
#else
    (void)render; (void)width; (void)height;
#endif
    return 0;
}

int evo_rmlui_twitch_fallback_active(void) {
#if defined(EVO_AGC_DEVICE)
    return evo_hui_start_failed() ? 1 : 0;
#else
    return 0;
#endif
}

void evo_rmlui_render_twitch_fallback(uint32_t* framebuffer, int width, int height, int focus) {
    EvoRmlApp::Instance().RenderTwitchFallback(framebuffer, width, height, focus);
}

void evo_rmlui_set_theme(const evo_rmlui_theme_t* t) {
    if (!t) return;
#if defined(EVO_AGC_DEVICE)
    evo_hui_set_theme(t);
#endif
    EvoThemeColors colors;
    colors.name = t->name ? t->name : "MIDNIGHT";
    colors.bg_top = t->bg_top;
    colors.bg_bottom = t->bg_bottom;
    colors.surface = t->surface;
    colors.surface_sel = t->surface_sel;
    colors.border = t->border;
    colors.border_sel = t->border_sel;
    colors.accent = t->accent;
    colors.accent_soft = t->accent_soft;
    colors.accent_alt = t->accent_alt;
    colors.text_primary = t->text_primary;
    colors.text_secondary = t->text_secondary;
    colors.text_muted = t->text_muted;
    EvoRmlApp::Instance().SetTheme(colors);
}

void evo_rmlui_update_nav(const evo_rmlui_nav_params_t* p) {
    if (!p) return;
#if defined(EVO_AGC_DEVICE)
    evo_hui_update_nav(p);
#endif
    EvoNavState state;
    state.active_section = p->active_section;
    state.rail_focused   = (p->rail_focused != 0);
    state.cursor_index   = p->cursor_index;
    state.visible        = (p->visible != 0);
    state.fps            = p->fps;
    state.show_fps       = p->show_fps;
    state.storage_locked = p->storage_locked;
    EvoRmlApp::Instance().UpdateNavState(state);
}

} // extern "C"

