#pragma once
#include <RmlUi/Core.h>
#include <cstring>
#include <string>
#include <vector>
#include <memory>
#include <utility>
#include <unordered_map>
#include "evo_rmlui_render.h"
#include "evo_rmlui_system.h"
#include "evo_rmlui_fileinterface.h"
#include "evo_rmlui_bridge.h"   /* evo_perf_hud_t */

struct EvoPlaybackState {
    std::string title;
    std::string meta;            // the line under it; a Twitch stream's "Category · 12.3K watching"
    std::string status;          // "Buffering"...: shown with the spinner
    double position_sec = 0.0;
    double duration_sec = 0.0;
    double percentage = 0.0;
    bool paused = false;
    bool scrub_active = false;
    double scrub_target = 0.0;
    bool loading = false;
    std::string quality;         // the badge at the top right and R2's hint; empty hides both
    bool is_live = false;
    int view_mode = 0; // 0=FIT, 1=FILL, 2=STRETCH
    bool show_stats = false;
    bool can_stats = false;      // a dev build: Options' STATS hint
    int alpha = 255;
    int fps = 0;                 // #81: dev FPS pill
    bool debug_overlay = false;
    bool music_mode = false;     // no picture: the "Audio only" card
    bool twitch = false;         // the Twitch keys in the hints
    int chat_mode = 0;           // EVO_OSD_CHAT_*
    bool can_chat = false;
    bool can_clip = false;       // Square makes a clip of a live stream
    bool twitch_video = false;   // a Twitch video or clip: a file's keys and timeline, no Triangle

    bool operator==(const EvoPlaybackState& o) const {
        return title == o.title && meta == o.meta && status == o.status &&
               position_sec == o.position_sec && duration_sec == o.duration_sec &&
               percentage == o.percentage && paused == o.paused &&
               scrub_active == o.scrub_active && scrub_target == o.scrub_target &&
               loading == o.loading &&
               quality == o.quality && is_live == o.is_live &&
               view_mode == o.view_mode && show_stats == o.show_stats && can_stats == o.can_stats &&
               alpha == o.alpha &&
               fps == o.fps && debug_overlay == o.debug_overlay &&
               music_mode == o.music_mode && twitch == o.twitch &&
               chat_mode == o.chat_mode && can_chat == o.can_chat && can_clip == o.can_clip &&
               twitch_video == o.twitch_video;
    }
    bool operator!=(const EvoPlaybackState& o) const { return !(*this == o); }
};

struct EvoDialogAction {
    std::string icon_path;
    std::string label;
    bool is_primary = false;

    bool operator==(const EvoDialogAction& o) const {
        return icon_path == o.icon_path && label == o.label && is_primary == o.is_primary;
    }
    bool operator!=(const EvoDialogAction& o) const { return !(*this == o); }
};

struct EvoDialogState {
    std::string eyebrow;
    std::string title;
    std::string detail;
    double progress_pct = -1.0; // 0.0 to 1.0, or -1.0 to hide
    int focused_action = -1;    // #65: D-pad-focused button index, -1 = none
    std::vector<EvoDialogAction> actions;

    bool operator==(const EvoDialogState& o) const {
        return eyebrow == o.eyebrow && title == o.title && detail == o.detail &&
               progress_pct == o.progress_pct && focused_action == o.focused_action &&
               actions == o.actions;
    }
    bool operator!=(const EvoDialogState& o) const { return !(*this == o); }
};

struct EvoSettingsRow {
    std::string title;
    std::string detail;
    std::string icon_path;
    std::string badge;
    bool has_chevron = true;
    bool is_focused = false;
    int  kind = 0;             /* EVO_RMLUI_ROW_* */
    bool toggle_on = false;
    bool is_disabled = false;

    bool operator==(const EvoSettingsRow& o) const {
        return title == o.title && detail == o.detail && icon_path == o.icon_path &&
               badge == o.badge && has_chevron == o.has_chevron && is_focused == o.is_focused &&
               kind == o.kind && toggle_on == o.toggle_on && is_disabled == o.is_disabled;
    }
    bool operator!=(const EvoSettingsRow& o) const { return !(*this == o); }
};

struct EvoSettingsState {
    std::string title;
    std::string subtitle;
    std::string counter;
    int rail_active_idx = 5;
    bool rail_focused = false;
    int section_active = -1;
    bool sidebar_focused = false;
    std::vector<EvoSettingsRow> rows;

    bool operator==(const EvoSettingsState& o) const {
        return title == o.title && subtitle == o.subtitle && counter == o.counter &&
               rail_active_idx == o.rail_active_idx && rail_focused == o.rail_focused &&
               section_active == o.section_active && sidebar_focused == o.sidebar_focused &&
               rows == o.rows;
    }
    bool operator!=(const EvoSettingsState& o) const { return !(*this == o); }
};

struct EvoAboutState {
    std::string app_name = "EVO PLAYER PRO";
    std::string version;
    std::string build_tag = "PS5 HOMEBREW";
    std::string tagline;
    std::string themes_info;
    bool action_focused = true;

    bool operator==(const EvoAboutState& o) const {
        return app_name == o.app_name && version == o.version &&
               build_tag == o.build_tag && tagline == o.tagline &&
               themes_info == o.themes_info && action_focused == o.action_focused;
    }
    bool operator!=(const EvoAboutState& o) const { return !(*this == o); }
};

struct EvoListRow {
    std::string title;
    std::string detail;
    std::string icon_path;
    std::string badge;
    int progress = -1;
    bool has_chevron = false;
    bool is_focused = false;

    bool operator==(const EvoListRow& o) const {
        return title == o.title && detail == o.detail && icon_path == o.icon_path &&
               badge == o.badge && progress == o.progress &&
               has_chevron == o.has_chevron && is_focused == o.is_focused;
    }
    bool operator!=(const EvoListRow& o) const { return !(*this == o); }
};

struct EvoListHint {
    std::string glyph_path;
    std::string label;

    bool operator==(const EvoListHint& o) const {
        return glyph_path == o.glyph_path && label == o.label;
    }
    bool operator!=(const EvoListHint& o) const { return !(*this == o); }
};

struct EvoListMenuRow {
    std::string label;
    std::string desc;
    std::string icon_path;
    bool danger = false;

    bool operator==(const EvoListMenuRow& o) const {
        return label == o.label && desc == o.desc && icon_path == o.icon_path &&
               danger == o.danger;
    }
    bool operator!=(const EvoListMenuRow& o) const { return !(*this == o); }
};

struct EvoListState {
    std::string title;
    std::string subtitle;
    int section = 0;
    bool rail_focused = false;

    int total_count = 0;
    int cursor_index = -1;
    std::vector<EvoListRow> rows;

    bool is_empty = false;
    std::string empty_title;
    std::string empty_hint;
    std::string empty_icon;

    std::vector<EvoListHint> hints;

    /* OPTIONS menu; empty = closed. */
    std::vector<EvoListMenuRow> menu;
    int menu_focus = 0;
    std::string menu_eyebrow;
    std::string menu_title;
    std::string menu_sub;
    std::string menu_icon;

    bool operator==(const EvoListState& o) const {
        return title == o.title && subtitle == o.subtitle && section == o.section &&
               rail_focused == o.rail_focused && total_count == o.total_count &&
               cursor_index == o.cursor_index && rows == o.rows &&
               is_empty == o.is_empty && empty_title == o.empty_title &&
               empty_hint == o.empty_hint && empty_icon == o.empty_icon && hints == o.hints &&
               menu == o.menu && menu_focus == o.menu_focus &&
               menu_eyebrow == o.menu_eyebrow && menu_title == o.menu_title &&
               menu_sub == o.menu_sub && menu_icon == o.menu_icon;
    }
    bool operator!=(const EvoListState& o) const { return !(*this == o); }
};


struct EvoThemeColors {
    /*
     * Packed 0xAABBGGRR, the same order EVO_RGBA() produces in evo_theme.c —
     * these are read back byte-for-byte by to_hex_rgb(). They used to be
     * written 0xAARRGGBB, which rendered MIDNIGHT's cyan accent as gold in
     * every frame drawn before main.c pushed the real theme, and in every
     * host uiview render, which never pushes one at all.
     */
    std::string name = "MIDNIGHT";
    uint32_t bg_top = 0xFF160D08;
    uint32_t bg_bottom = 0xFF0A0503;
    uint32_t surface = 0xF02C1A10;
    uint32_t surface_sel = 0xFA3A2216;
    uint32_t border = 0x33B47D5A;
    uint32_t border_sel = 0xFF00CDFF;
    uint32_t accent = 0xFF00CDFF;
    uint32_t accent_soft = 0x3200CDFF;
    uint32_t accent_alt = 0xFFFFCD00;
    uint32_t text_primary = 0xFFFFF4EE;
    uint32_t text_secondary = 0xFFD0B6A3;
    uint32_t text_muted = 0xFF8C715E;
};

struct EvoNavState {
    int active_section = 0; /* ScreenManager's sections: 0 Home 1 Following 2 Browse 3 Search 4 Settings */
    int rail_focused = 0;   /* 0=collapsed icon strip, 1=expanded labelled panel */
    int cursor_index = 0;   /* which item has cursor when expanded (5 is QUIT) */
    int visible = 1;        /* 1=show the nav rail, 0=hide (full-screen OSD etc.) */
    int fps = 0;            /* render FPS shown in the rail pill */
    int show_fps = 0;       /* DEBUG OVERLAY setting */
    int storage_locked = 0; /* 1 = sandbox never lifted; show the banner */

    bool operator==(const EvoNavState& o) const {
        return active_section == o.active_section && rail_focused == o.rail_focused &&
               cursor_index == o.cursor_index && visible == o.visible &&
               fps == o.fps && show_fps == o.show_fps &&
               storage_locked == o.storage_locked;
    }
    bool operator!=(const EvoNavState& o) const { return !(*this == o); }
};

/*
 * #75: toast notifications. Lives in its OWN Rml::Context (m_toast_context),
 * separate from the menu/OSD documents in m_context - a toast has to
 * composite on top of whichever screen is active (menu, playback OSD,
 * dialog, ...) without disturbing that screen's own Show()/Hide() state or,
 * for cached menu screens (RenderCachedScreen), forcing a full re-rasterise
 * on every animation frame the way sharing the main context would. Its own
 * context is cheap to render every frame regardless - the document is a
 * single small card, nothing like a full 1920x1080 screen.
 */
struct EvoToastState {
    std::string title;
    std::string message;
    int kind = 0;        /* 0=info 1=tech 2=error 3=ok - see evo_rmlui_bridge.h */
    bool visible = false;
    int alpha = 0;        /* 0..255, caller owns the fade (matches the old evo_toast timing) */
    int slide = 0;        /* px still to travel on the way in */
};

class EvoRmlApp {

public:
    static EvoRmlApp& Instance();

    bool Initialize(int width, int height);
    void Shutdown();

    void SetTheme(const EvoThemeColors& theme);
    void SetVersion(const std::string& v) { m_version = v; }
    const EvoThemeColors& GetTheme() const { return m_theme; }

    void UpdateListState(const EvoListState& state);
    void RenderList(uint32_t* framebuffer, int width, int height);

    void UpdatePlaybackState(const EvoPlaybackState& state);
    void UpdatePerfHud(const evo_perf_hud_t* hud);   // #81/#63 diagnostic HUD
    void RenderPlaybackOSD(uint32_t* framebuffer, int width, int height);

    void UpdateDialogState(const EvoDialogState& state);
    void RenderDialog(uint32_t* framebuffer, int width, int height);

    void UpdateSettingsState(const EvoSettingsState& state);
    void RenderSettings(uint32_t* framebuffer, int width, int height);

    void UpdateAboutState(const EvoAboutState& state);
    void RenderAbout(uint32_t* framebuffer, int width, int height);
    /* Safe-to-close screen (closed.rml): stateless, themed on every render. */
    void RenderClosed(uint32_t* framebuffer, int width, int height);
    /* What the Twitch screens show when the kit could not start
     * (twitch_fallback.rml). focus: the button the cursor is on, 0 Open
     * Settings, 1 Quit. */
    void RenderTwitchFallback(uint32_t* framebuffer, int width, int height, int focus);

    /* Sidebar navigation rail — call UpdateNavState before any Render* call */
    void UpdateNavState(const EvoNavState& state);

    /*
     * #75: toast notifications - own context (see EvoToastState), composited
     * over whatever the caller already rendered into framebuffer this frame.
     * Call RenderToast() AFTER the screen's own Render*() call, every frame
     * evo_toast.c's timing considers the toast live (matches the old
     * draw_prospero_toast(fb) call site in main.c exactly).
     */
    void UpdateToastState(const EvoToastState& state);
    void SetOverlayDimensions(Rml::Context* ctx, int width, int height);
    void RenderToast(uint32_t* framebuffer, int width, int height);

    /* #81: virtual keyboard modal - own context, composited over the screen. */
    void UpdateKeyboard(const evo_keyboard_params_t* p);
    void RenderKeyboard(uint32_t* framebuffer, int width, int height);

    /*
     * Dev debug overlay - the menu-screen FPS pill (GL-5 kept it player-only;
     * this restores it for every other screen). Own context, same convention as
     * the toast: call UpdateDebugOverlay() then RenderDebugOverlay() AFTER the
     * screen's own Render* call. `visible` follows show_debug_overlay; when it
     * is up GlNeedsFrame() ticks ~2 Hz so the number keeps counting on an
     * otherwise-idle (change-gated) menu.
     */
    void UpdateDebugOverlay(int fps, bool visible);
    void RenderDebugOverlay(uint32_t* framebuffer, int width, int height);

    bool IsInitialized() const { return m_initialized; }

    /* Mark the cached menu surface stale, so it gets re-rasterised by the
     * device loop's next active frame. */
    void MarkFrameDirty() { m_frame_dirty = true; }

private:
    EvoRmlApp();
    ~EvoRmlApp();

    bool m_initialized = false;
    int m_width = 1920;
    int m_height = 1080;

    std::unique_ptr<EvoSystemInterface> m_system;
    /* The active RmlUi render interface, behind the EvoRenderBridge seam:
     * EvoRenderInterface (CPU coverage rasteriser, the device path) or, on the
     * host with EVO_RML_GL set, EvoRenderInterfaceGL (RmlUi's GL3 backend -
     * render-overhaul GL-2, #78). */
    std::unique_ptr<EvoRenderBridge> m_render;
    /* #60: serves .rml/.rcss/.ttf out of the embedded bundle - see
     * evo_rmlui_fileinterface.h. Owned here so it outlives Rml::Initialise(),
     * which only stores the raw pointer Rml::SetFileInterface() is given. */
    std::unique_ptr<EvoRmlFileInterface> m_file_interface;
    Rml::Context* m_context = nullptr;
    /* panel width / EVO_UI_DESIGN_W; 1.0 at 1080p. Applied to every context. */
    float m_dp_ratio = 1.0f;
    Rml::ElementDocument* m_list_doc = nullptr;
    Rml::ElementDocument* m_playback_doc = nullptr;
    Rml::ElementDocument* m_dialog_doc = nullptr;
    Rml::ElementDocument* m_settings_doc = nullptr;
    Rml::ElementDocument* m_about_doc = nullptr;
    Rml::ElementDocument* m_closed_doc = nullptr;
    Rml::ElementDocument* m_twitch_fallback_doc = nullptr;
    int m_fallback_focus = -1;               /* last drawn, -1 before the first */
    int m_theme_gen_fallback = -1;
    Rml::ElementDocument* m_nav_doc = nullptr;

    /* #75: separate context - see the comment on EvoToastState. Content
     * (title/message/kind) is diffed separately from the per-frame
     * alpha/slide animation values, which always re-apply - see
     * UpdateToastState. */
    Rml::Context* m_toast_context = nullptr;
    Rml::ElementDocument* m_toast_doc = nullptr;
    std::string m_toast_last_title;
    std::string m_toast_last_message;
    int m_toast_last_kind = -1;

    /* #81: virtual keyboard - own context (same rationale as the toast). */
    Rml::Context* m_keyboard_context = nullptr;
    Rml::ElementDocument* m_keyboard_doc = nullptr;
    std::string m_kb_sig;   /* cheap change gate for UpdateKeyboard */

    /* Dev debug overlay (menu FPS pill) - own context, same rationale. */
    Rml::Context* m_debug_context = nullptr;
    Rml::ElementDocument* m_debug_doc = nullptr;
    bool m_debug_visible = false;
    int  m_debug_last_fps = -1;
    long long m_debug_tick_ms = 0;   /* GlNeedsFrame ~2 Hz refresh timer */

    EvoThemeColors m_theme;
    std::string m_version;
    EvoListState m_last_list;
    EvoPlaybackState m_last_state;

    /*
     * Bumped on every SetTheme call. Each Update*State call skips its whole
     * body - all the SetProperty/SetInnerRML work - when the incoming state
     * struct equals the one from last frame AND the theme has not moved
     * since that frame was processed. Without the generation check, a screen
     * sitting idle (state unchanged) would miss a live theme switch until
     * something else about it also happened to change.
     *
     * This exists because RmlUi's SetProperty always marks its target dirty
     * regardless of whether the value actually changed, forcing a full
     * geometry/decorator rebuild - so calling it on unchanged state every
     * single frame (the previous behavior here) measured 6-7 fps on the
     * home screen on real hardware, slow enough that the pad is polled too
     * rarely to reliably catch a normal button tap.
     */
    int m_theme_generation = 0;
    int m_theme_gen_list = -1;
    int m_theme_gen_playback = -1;
    int m_theme_gen_dialog = -1;
    int m_theme_gen_settings = -1;
    int m_theme_gen_about = -1;
    int m_theme_gen_nav = -1;

    /* Rows the list document actually contains. Must stay in step with
     * EVO_RMLUI_LIST_ROWS in evo_rmlui_bridge.h and with list.rml. */
    static const int kListRows = 9;
    static const int kListMenuRows = 7;   /* EVO_RMLUI_LIST_MENU_ROWS */

    /*
     * RmlUi caches textures by source string, so a picture whose pixels change
     * has to change name too or the stale frame is served forever. Each art
     * slot carries a generation that bumps when its pixels do, and the
     * previous generation is released on the way.
     *
     * One slot: the scrub preview (#32), the frame prospero_thumbnail decoded
     * for the position under the play head while the seek bar is dragged.
     */
    static const int kScrubArtSlot = 0;
    static const int kArtSlots = 1;
    int m_art_generation[kArtSlots] = {};
    const uint32_t* m_art_last_ptr[kArtSlots] = {};
    int m_art_last_dims[kArtSlots][2] = {};
    /* A reused buffer is not unchanged pixels: the caller's tag rides along
     * as the content tag. */
    std::string m_art_last_tag[kArtSlots];
    std::string m_art_source[kArtSlots];
    std::string ArtSource(int slot, const uint32_t* pixels, int w, int h,
                          const std::string& tag);

    /* Scrub preview state: the decoded frame staged for upload, and the
     * worker's serial for the frame already uploaded. See UpdateScrubPreview()
     * in evo_rmlui_app.cpp. */
    std::vector<uint32_t> m_scrub_thumb_pixels;
    unsigned long long    m_scrub_thumb_serial = 0;
    /* The worker's serial as the drag began. Anything at or below it was
     * decoded for a previous scrub, so showing it would put a frame from the
     * wrong position under the timecode for the first frame or two. */
    unsigned long long    m_scrub_thumb_baseline = 0;
    bool                  m_scrub_thumb_shown = false;
    void UpdateScrubPreview(const EvoPlaybackState& state);
    EvoDialogState m_last_dialog;
    EvoSettingsState m_last_settings;
    EvoAboutState m_last_about;
    EvoNavState m_last_nav;

    /*
     * RmlUi's ElementImage::OnPropertyChange marks geometry dirty on ANY
     * image-color SetProperty call, regardless of whether the value actually
     * changed - SetProperty itself never diffs against the current value.
     * Icon tinting is recomputed every frame (nav rail runs every screen,
     * every frame, via evo_sync_rmlui_nav), so without this cache every icon
     * rebuilds its geometry every frame even when its colour is unchanged
     * from the last one - the cost that made the navbar miss D-pad presses.
     */
    std::unordered_map<Rml::Element*, std::string> m_image_color_cache;
    void SetImageColor(Rml::Element* el, const std::string& color);

    /*
     * Long-text marquee, matching the legacy evo_text_marquee ping-pong. Only
     * used on the playback OSD title (#media-title): that document is not in
     * the Step-1 surface cache, so scrolling it costs nothing extra, whereas
     * marqueeing a cached menu row would force a full re-raster every frame.
     * The menu/list rows keep the static text-overflow: ellipsis in their RCSS.
     */
    struct MarqueeState { bool active = false; double start = 0.0; float travel = 0.0f; };
    std::unordered_map<Rml::Element*, MarqueeState> m_marquee;
    void MarqueeTick(Rml::Element* el, bool active);

    /*
     * State-diffing in Update*State only avoids RE-STYLING unchanged
     * elements; it does nothing about the cost of Context::Render() itself,
     * which walks and CPU-rasterizes every visible element every time it is
     * called regardless of whether anything is dirty - measured at 6-7 fps
     * on the launch screen with state-diffing already in place and skipping
     * ~97% of Update calls (host profile: Context::Update 0.1 ms, the whole
     * ~120 ms frame is the render-interface raster). That render pass is what
     * needs skipping on an unchanged frame.
     *
     * Step 1 of docs/evo-pro/gpu-rendering-plan.md: the full-screen menu
     * documents (list, settings) are opaque, cover the whole 1080p
     * frame, and - the RCSS carries no transitions or animations - never
     * change between input events. Each rasterizes into m_surface ONLY when m_frame_dirty (a real
     * state / theme / nav change set it), the visible screen switched, or the
     * surface was (re)sized; every frame then blits m_surface into the
     * caller's rotating VideoOut buffer (~0.5 ms memcpy). The blitted pixels
     * are exactly what Context::Render() last produced, so parity holds.
     *
     * Overlay screens that compose over live video (playback OSD, dialog)
     * are NOT cached - they keep rendering
     * straight into the framebuffer every frame, unchanged.
     */
    bool m_frame_dirty = true;
    std::vector<uint32_t> m_surface;
    int m_surface_w = 0;
    int m_surface_h = 0;
    int m_cached_screen = -1;
    void RenderCachedScreen(int screen_id, uint32_t* framebuffer, int width, int height);

    /*
     * Show `keep` and hide every other full-screen document in the main context.
     *
     * Each Render* used to carry its own hand-maintained hide list, and adding a
     * screen meant remembering to add it to twelve of them. EVO's image viewer
     * document (#81, since removed) was added to none: it stayed visible under
     * every later screen, invisible behind an opaque menu but painting the whole frame
     * through the playback OSD's transparent background - so starting a video
     * after viewing an image showed the image instead of the film. One list, in
     * one place, is the fix for the class rather than the instance.
     *
     * The nav rail is deliberately not included: it is a companion document
     * whose visibility comes from UpdateNavState, and each caller handles it.
     */
    void ShowOnlyScreen(Rml::ElementDocument* keep);

public:
    /* GL-3 (#79) B2 device loop. Retained-mode + ps5-opengl can't re-raster at
     * 60 Hz, so the loop only redraws + swaps on change. Per frame, main.c:
     *   a = GlNeedsFrame();          // dirty || overlay visible || warm-up
     *   GlSetActive(a);              // gate for RenderCachedScreen this frame
     *   ... run the dispatch (update calls always run, so input sets dirty) ...
     *   if (a && GlConsumeDrew()) { present(); GlEndFrame(); }  // clears dirty
     */
    bool GlNeedsFrame();
    void GlSetActive(bool a) { m_gl_active = a; }
    bool GlActive() const { return m_gl_active; }
    bool GlConsumeDrew() { bool d = m_drew; m_drew = false; return d; }
    /* A ps5-homebrew-ui screen (ui_kit/) drew into this frame instead. */
    void NoteExternalDraw() {
        m_drew = true;
        /* The kit screen replaces whatever RmlUi screen was up - including a
         * dialog left visible by the screen we came from. A modal rendered
         * after it this frame shows its own document again. */
        ShowOnlyScreen(nullptr);
        if (m_nav_doc) m_nav_doc->Hide();
    }
    /* A kit overlay drew over this frame's screen: only RmlUi's own copy of
     * that overlay goes away; the screen under it is left as it is. */
    void NoteExternalDialog() {
        m_drew = true;
        if (m_dialog_doc) m_dialog_doc->Hide();
    }
    void NoteExternalKeyboard() { m_drew = true; }
    void NoteExternalToast() {
        m_drew = true;
        if (m_toast_doc) m_toast_doc->Hide();
    }
    void GlEndFrame() { m_frame_dirty = false; }
    /* true  = CPU coverage rasteriser + one GL blit (default; ps5-opengl's fast
     *         path). false = RmlUi renders itself through RenderInterface_GL3
     *         (selectable via /mnt/usb0/evo_gl_rmlui - slow on ps5-opengl G47,
     *         kept for when the driver's render-to-texture path improves). */
    bool GlBlitMode() const { return m_blit_mode; }
private:
    int  m_gl_warmup = 3;
    bool m_drew = false;
    bool m_gl_active = false;
    bool m_blit_mode = true;
};

