/*
 * evo_hui_app.cpp - runs EVO's ps5-homebrew-ui screens on the console.
 *
 * Owns the AGC batch, the fonts and the screen objects. Started lazily on the
 * first kit render, which is always inside an AGC frame on the main thread, so
 * the runtime and ui_sdf.pipe are known to be up. If anything fails the kit
 * stays off for the session and every screen keeps its RmlUi version.
 */
#include "evo_hui.h"

#include "evo_hui_extra.hpp"
#include "evo_hui_misc.hpp"
#include "evo_hui_osd.hpp"
#include "evo_hui_screens.hpp"
#include "evo_hui_settings.hpp"
#include "evo_hui_twitch.hpp"
#include "hui_agc_batch.hpp"

#include "evo_agc_runtime.h"
#include "evo_boot_log.h"
#include "evo_rmlui_bundle.h"

#include "gfx/font.hpp"
#include "ui/fonts.hpp"

#include <time.h>
#include <unistd.h>

#include <cstdio>
#include <memory>
#include <string>

namespace
{

using evo::kit::Context;

class BundleTextures final : public evo::kit::TextureHost
{
  public:
    explicit BundleTextures(evo::hui_agc::AgcBatch &batch) : batch_(batch) {}
    std::uint32_t create_texture(int w, int h, const std::uint8_t *rgba) override
    {
        return batch_.create_texture(w, h, rgba);
    }
    void release_texture(std::uint32_t handle) override
    {
        batch_.release_texture(handle);
    }
    bool read_asset(const std::string &path, std::string *bytes) override
    {
        /* Screens use the RmlUi documents' relative paths ("../icons/x.png");
         * bundle keys are relative to assets/. */
        std::string key = path;
        while (key.compare(0, 3, "../") == 0)
            key.erase(0, 3);
        const EvoRmlBundleFile *f = evo_rmlui_bundle_find(key);
        if (!f)
            return false;
        bytes->assign(reinterpret_cast<const char *>(f->data), f->size);
        return true;
    }

  private:
    evo::hui_agc::AgcBatch &batch_;
};

double now_seconds()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

enum class Screen
{
    none,
    list,
    settings,
    osd,
    about,
    closed,
    twitch_home,
    twitch_browse,
    twitch_category,
    twitch_search,
    twitch_channel,
    twitch_following,
    twitch_login,
    twitch_chat, /* over the video, and over the OSD when it is up */
    dialog, /* overlays from here on */
    toast,
    keyboard,
    twitch_clip, /* the card for a clip made in the player */
    count,
};

const char *doc_name(Screen s)
{
    switch (s)
    {
    case Screen::list:
        return "hui_list";
    case Screen::settings:
        return "hui_settings";
    case Screen::osd:
        return "hui_osd";
    case Screen::about:
        return "hui_about";
    case Screen::closed:
        return "hui_closed";
    case Screen::twitch_home:
        return "hui_twitch_home";
    case Screen::twitch_browse:
        return "hui_twitch_browse";
    case Screen::twitch_category:
        return "hui_twitch_category";
    case Screen::twitch_search:
        return "hui_twitch_search";
    case Screen::twitch_channel:
        return "hui_twitch_channel";
    case Screen::twitch_following:
        return "hui_twitch_following";
    case Screen::twitch_login:
        return "hui_twitch_login";
    case Screen::twitch_chat:
        return "hui_twitch_chat";
    default:
        return "hui";
    }
}

struct Kit
{
    enum State
    {
        untried,
        up,
        failed,
    } state = untried;

    evo::hui_agc::AgcBatch batch;
    std::unique_ptr<BundleTextures> host;
    std::unique_ptr<evo::kit::TextureCache> textures;
    hui::gfx::Font font_regular, font_semibold, font_display, font_mono;
    hui::ui::Fonts fonts;
    evo::kit::Palette palette;

    evo::kit::NavRail rail;
    evo::kit::ListScreen list;
    evo::kit::SettingsScreen settings;
    evo::kit::OsdScreen osd;
    evo::kit::AboutScreen about;
    evo::kit::ClosedScreen closed;
    evo::kit::DialogOverlay dialog;
    evo::kit::ToastOverlay toast;
    evo::kit::KeyboardOverlay keyboard;
    evo::kit::TwitchHomeScreen twitch_home;
    evo::kit::TwitchBrowseScreen twitch_browse;
    evo::kit::TwitchCategoryScreen twitch_category;
    evo::kit::TwitchSearchScreen twitch_search;
    evo::kit::TwitchChannelScreen twitch_channel;
    evo::kit::TwitchFollowingScreen twitch_following;
    evo::kit::TwitchLoginScreen twitch_login;
    evo::kit::TwitchChatScreen twitch_chat;
    evo::kit::TwitchClipCard twitch_clip;
    bool keyboard_visible = false;
    /* Frame bookkeeping: one dt per presented frame, however many kit draws
     * it has (a screen, then a dialog over it). */
    unsigned frame_serial = 0;
    unsigned stepped_serial = ~0u;
    /* The frame the last render() drew in and the screen it drew there (not
     * chat when chat only drew over it), and what `current` was before chat
     * took it in a frame chat drew first in. */
    unsigned rendered_serial = ~0u;
    Screen rendered_last = Screen::none;
    Screen chat_replaced = Screen::none;
    float frame_dt = 0.0f;
    /* Frame each overlay last drew in; far in the past to begin with. */
    unsigned overlay_serial[static_cast<int>(Screen::count)] = {};
    bool dialog_up = false;

    /* evo_hui_set_osd_area: the OSD's canvas in canvas units, p * scale +
     * (dx, dy). The whole screen until the player says otherwise. */
    float osd_scale = 1.0f, osd_dx = 0.0f, osd_dy = 0.0f;

    Screen current = Screen::none;
    Screen drawn_last_frame = Screen::none;
    bool start_failed = false; /* start() tried and failed (not a shutdown) */
    int idle_frames = 0;
    double last_time = 0.0;
    float clock = 0.0f;

    bool load_font(const char *path, hui::gfx::Font *font, hui::ui::FontRef *ref)
    {
        const EvoRmlBundleFile *f = evo_rmlui_bundle_find(path);
        if (!f || !font->load(std::string_view(reinterpret_cast<const char *>(f->data), f->size)))
        {
            evo_boot_log("hui: font %s missing or unreadable", path);
            return false;
        }
        ref->font = font;
        ref->texture = batch.create_font_texture(*font);
        if (!ref->texture)
        {
            evo_boot_log("hui: font %s upload failed", path);
            return false;
        }
        return true;
    }

    bool start()
    {
        if (state != untried)
            return state == up;
        state = failed;
        for (unsigned &s : overlay_serial)
            s = 0x80000000u;
        /* EVO's kill switch, /mnt/usb0/evo_no_hui, is not honoured here: the
         * Twitch screens exist only in the kit, so a USB stick that still
         * carries the file from EVO would leave this app blank. */
        if (!evo_agc_runtime_is_active())
        {
            evo_boot_log("hui: kit screens could not start: the GPU runtime is not up");
            start_failed = true;
            return false;
        }
        if (!batch.init())
        {
            evo_boot_log("hui: kit screens could not start: the draw batch did not initialise");
            start_failed = true;
            return false;
        }
        if (!load_font("hui/fonts/inter-regular.huifont", &font_regular, &fonts.regular) ||
            !load_font("hui/fonts/inter-semibold.huifont", &font_semibold, &fonts.semibold) ||
            !load_font("hui/fonts/montserrat-medium.huifont", &font_display, &fonts.display) ||
            !load_font("hui/fonts/dejavu-sans-mono.huifont", &font_mono, &fonts.mono))
        {
            evo_boot_log("hui: kit screens could not start: a font did not load");
            batch.release();
            start_failed = true;
            return false;
        }
        fonts.pixel = fonts.mono;
        fonts.hand = fonts.regular;
        host = std::make_unique<BundleTextures>(batch);
        textures = std::make_unique<evo::kit::TextureCache>(*host);
        last_time = now_seconds();
        state = up;
        evo_boot_log("hui: kit screens up");
        return true;
    }

    /* Advances the clock once per frame, however many kit draws it has. */
    float step()
    {
        if (stepped_serial == frame_serial)
            return frame_dt;
        stepped_serial = frame_serial;
        const double t = now_seconds();
        float dt = static_cast<float>(t - last_time);
        last_time = t;
        if (dt < 0.0f)
            dt = 0.0f;
        if (dt > 0.1f)
            dt = 0.1f; /* a stall must not throw springs across the screen */
        clock += dt;
        frame_dt = dt;
        return dt;
    }

    /* Records `screen` into the current AGC frame. */
    template <typename DrawFn>
    int render(Screen screen, int width, int height, DrawFn &&draw_screen, bool with_rail = true)
    {
        if (!start())
            return 0;
        /* Chat and the OSD can both draw in a frame, in either order, and
         * the OSD stays in charge: chat taking `current` would make it enter
         * afresh on every frame. So chat drawn after another screen only
         * draws over it, and a screen drawn after chat takes `current` back
         * as chat found it. Drawn alone, chat is the screen. */
        const bool same_frame = rendered_serial == frame_serial;
        const bool chat = screen == Screen::twitch_chat;
        const bool over = chat && same_frame && rendered_last != Screen::twitch_chat;
        if (!chat && same_frame && rendered_last == Screen::twitch_chat)
            current = chat_replaced;
        if (chat && !over)
            chat_replaced = current;
        rendered_serial = frame_serial;
        if (!over)
            rendered_last = screen;
        if (!over && current != screen)
        {
            current = screen;
            if (screen == Screen::list)
                list.enter();
            else if (screen == Screen::settings)
                settings.enter();
            else if (screen == Screen::osd)
                osd.enter();
            else if (screen == Screen::about)
                about.enter();
            else if (screen == Screen::closed)
                closed.enter();
            else if (screen == Screen::twitch_home)
                twitch_home.enter();
            else if (screen == Screen::twitch_browse)
                twitch_browse.enter();
            else if (screen == Screen::twitch_category)
                twitch_category.enter();
            else if (screen == Screen::twitch_search)
                twitch_search.enter();
            else if (screen == Screen::twitch_channel)
                twitch_channel.enter();
            else if (screen == Screen::twitch_following)
                twitch_following.enter();
            else if (screen == Screen::twitch_login)
                twitch_login.enter();
            else if (screen == Screen::twitch_chat)
                twitch_chat.enter();
        }
        const float dt = step();
        if (!chat)
            rail.update(dt); /* chat never shows the rail */
        if (!over)
            drawn_last_frame = screen;
        idle_frames = 0;

        evo_agc_runtime_frame_begin();
        Context ctx{fonts, *textures, palette, clock};
        hui::gfx::DrawList &list = scratch;
        list.clear();
        draw_screen(dt, list, ctx);
        if (with_rail)
            rail.draw(list, ctx);
        batch.draw(list, viewport(screen, width, height), width, height);
        return 1;
    }

    /* The canvas on the surface: letterboxed into the whole of it, except
     * the OSD, which goes into its area (evo_hui_set_osd_area). */
    hui::gfx::Viewport viewport(Screen screen, int width, int height) const
    {
        hui::gfx::Viewport v = hui::gfx::fit_viewport(width, height);
        if (screen != Screen::osd || (osd_scale == 1.0f && osd_dx == 0.0f && osd_dy == 0.0f))
            return v;
        hui::gfx::Viewport area;
        area.scale = v.scale * osd_scale;
        area.offset_x = v.offset_x + osd_dx * v.scale;
        area.offset_y = v.offset_y + osd_dy * v.scale;
        return area;
    }

    /* An overlay over this frame's screen: leaves `current` alone, and runs
     * the overlay's entrance when it was not up on the previous frame. */
    template <typename DrawFn>
    int render_overlay(Screen overlay, int width, int height, DrawFn &&draw_overlay)
    {
        if (!start())
            return 0;
        const int slot = static_cast<int>(overlay);
        /* Not drawn this frame or the one before: it has just appeared. */
        const bool fresh = overlay_serial[slot] != frame_serial && overlay_serial[slot] + 1 != frame_serial;
        if (fresh)
        {
            if (overlay == Screen::dialog)
                dialog.enter();
            else if (overlay == Screen::toast)
                toast.enter();
            else if (overlay == Screen::keyboard)
                keyboard.enter();
            else if (overlay == Screen::twitch_clip)
                twitch_clip.enter();
        }
        overlay_serial[slot] = frame_serial;
        const float dt = step();
        if (overlay == Screen::dialog)
            dialog_up = true;

        evo_agc_runtime_frame_begin();
        Context ctx{fonts, *textures, palette, clock};
        hui::gfx::DrawList &list = overlay_scratch;
        list.clear();
        draw_overlay(dt, list, ctx);
        batch.draw(list, hui::gfx::fit_viewport(width, height), width, height);
        return 1;
    }

    hui::gfx::DrawList scratch;
    hui::gfx::DrawList overlay_scratch;
};

Kit &kit()
{
    /* Never destroyed: shutdown is explicit, like EvoRmlApp's. */
    static Kit *k = new Kit();
    return *k;
}

} // namespace

extern "C" {

void evo_hui_shutdown(void)
{
    Kit &k = kit();
    if (k.state != Kit::up)
        return;
    k.textures.reset();
    k.host.reset();
    k.batch.release();
    k.state = Kit::failed;
}

void evo_hui_set_osd_area(float x, float y, float w, float h)
{
    Kit &k = kit();
    evo_hui_osd_area_fit(x, y, w, h, &k.osd_scale, &k.osd_dx, &k.osd_dy);
}

int evo_hui_start_failed(void)
{
    return kit().start_failed ? 1 : 0;
}

int evo_hui_wants_frame(void)
{
    Kit &k = kit();
    return (k.state == Kit::up && k.drawn_last_frame != Screen::none) ? 1 : 0;
}

void evo_hui_end_frame(void)
{
    Kit &k = kit();
    if (k.state != Kit::up)
        return;
    k.batch.end_frame();
    ++k.frame_serial;
    k.textures->tick();
    /* A frame in which no kit screen drew means an RmlUi screen took over. */
    if (++k.idle_frames > 1)
    {
        k.drawn_last_frame = Screen::none;
        k.current = Screen::none;
    }
}

void evo_hui_set_theme(const evo_rmlui_theme_t *theme)
{
    if (theme)
        kit().palette = evo::kit::Palette::from(*theme);
}

void evo_hui_update_nav(const evo_rmlui_nav_params_t *params)
{
    if (params)
        kit().rail.set(*params);
}

const char *evo_hui_dev_focus_json(const char **screen_doc)
{
    Kit &k = kit();
    if (k.state != Kit::up || k.drawn_last_frame == Screen::none)
        return nullptr;
    const char *doc = doc_name(k.drawn_last_frame);
    if (screen_doc)
        *screen_doc = doc;

    evo::kit::FocusInfo f;
    const bool dialog_shown = k.overlay_serial[static_cast<int>(Screen::dialog)] + 1 >= k.frame_serial;
    const bool kb_shown = k.overlay_serial[static_cast<int>(Screen::keyboard)] + 1 >= k.frame_serial;
    bool has = kb_shown && k.keyboard.focus(&f);
    if (has && screen_doc)
        *screen_doc = "hui_keyboard";
    if (!has)
        has = dialog_shown && k.dialog.focus(&f);
    if (has && screen_doc)
        *screen_doc = "hui_dialog";
    if (!has)
        has = k.rail.focus(&f);
    if (!has && k.drawn_last_frame == Screen::list)
        has = k.list.focus(&f);
    else if (!has && k.drawn_last_frame == Screen::settings)
        has = k.settings.focus(&f);
    else if (!has && k.drawn_last_frame == Screen::osd)
        has = k.osd.focus(&f);
    else if (!has && k.drawn_last_frame == Screen::about)
        has = k.about.focus(&f);
    else if (!has && k.drawn_last_frame == Screen::twitch_home)
        has = k.twitch_home.focus(&f);
    else if (!has && k.drawn_last_frame == Screen::twitch_browse)
        has = k.twitch_browse.focus(&f);
    else if (!has && k.drawn_last_frame == Screen::twitch_category)
        has = k.twitch_category.focus(&f);
    else if (!has && k.drawn_last_frame == Screen::twitch_search)
        has = k.twitch_search.focus(&f);
    else if (!has && k.drawn_last_frame == Screen::twitch_channel)
        has = k.twitch_channel.focus(&f);
    else if (!has && k.drawn_last_frame == Screen::twitch_following)
        has = k.twitch_following.focus(&f);
    else if (!has && k.drawn_last_frame == Screen::twitch_login)
        has = k.twitch_login.focus(&f);
    else if (!has && k.drawn_last_frame == Screen::twitch_chat)
        has = k.twitch_chat.focus(&f);
    static std::string o;
    o.clear();
    if (!has)
    {
        o = "null";
        return o.c_str();
    }
    auto str = [&](const std::string &s) {
        o += '"';
        for (unsigned char c : s)
        {
            if (c == '"' || c == '\\')
            {
                o += '\\';
                o += static_cast<char>(c);
            }
            else if (c < 0x20)
            {
                char b[8];
                std::snprintf(b, sizeof b, "\\u%04x", c);
                o += b;
            }
            else
            {
                o += static_cast<char>(c);
            }
        }
        o += '"';
    };
    o += "{\"id\":";
    str(f.id);
    o += ",\"tag\":\"hui\",\"class\":\"focused\",\"text\":";
    str(f.text);
    o += ",\"doc\":";
    str(doc);
    char b[160];
    std::snprintf(b, sizeof b,
                  ",\"index\":%d,\"total\":%d,\"rect\":{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}",
                  f.index, f.total, static_cast<int>(f.rect.x), static_cast<int>(f.rect.y),
                  static_cast<int>(f.rect.w), static_cast<int>(f.rect.h));
    o += b;
    o += ",\"focusable\":[";
    for (std::size_t i = 0; i < f.focusable.size(); ++i)
    {
        if (i)
            o += ',';
        o += "{\"id\":";
        str(f.focusable[i].first);
        o += ",\"text\":";
        str(f.focusable[i].second);
        o += '}';
    }
    o += "]}";
    return o.c_str();
}

void evo_hui_update_list(const evo_rmlui_list_params_t *params)
{
    if (params)
        kit().list.set(*params);
}

int evo_hui_render_list(int width, int height)
{
    Kit &k = kit();
    return k.render(Screen::list, width, height,
                    [&](float dt, hui::gfx::DrawList &list, const Context &ctx) {
                        k.list.update(dt);
                        k.list.draw(list, ctx);
                    });
}

void evo_hui_update_settings(const evo_rmlui_settings_params_t *params)
{
    if (params)
        kit().settings.set(*params);
}

int evo_hui_render_settings(int width, int height)
{
    Kit &k = kit();
    return k.render(Screen::settings, width, height,
                    [&](float dt, hui::gfx::DrawList &list, const Context &ctx) {
                        k.settings.update(dt);
                        k.settings.draw(list, ctx);
                    });
}

void evo_hui_update_playback(const evo_playback_osd_params_t *params)
{
    if (params)
        kit().osd.set(*params);
}

void evo_hui_update_perf_hud(const evo_perf_hud_t *hud)
{
    if (hud)
        kit().osd.set_hud(*hud);
}

int evo_hui_render_playback_osd(int width, int height)
{
    Kit &k = kit();
    if (!k.start())
        return 0;
    /* Over the video: no rail, no backdrop. */
    return k.render(
        Screen::osd, width, height,
        [&](float dt, hui::gfx::DrawList &list, const Context &ctx) {
            k.osd.update(dt);
            k.osd.draw(list, ctx);
        },
        false);
}

void evo_hui_update_about(const evo_rmlui_about_params_t *params)
{
    if (params)
        kit().about.set(*params);
}

int evo_hui_render_about(int width, int height)
{
    Kit &k = kit();
    return k.render(Screen::about, width, height,
                    [&](float dt, hui::gfx::DrawList &list, const Context &ctx) {
                        k.about.update(dt);
                        k.about.draw(list, ctx);
                    });
}

int evo_hui_render_closed(int width, int height)
{
    Kit &k = kit();
    return k.render(
        Screen::closed, width, height,
        [&](float dt, hui::gfx::DrawList &list, const Context &ctx) {
            k.closed.update(dt);
            k.closed.draw(list, ctx);
        },
        false);
}

void evo_hui_update_dialog(const evo_rmlui_dialog_params_t *params)
{
    if (params)
        kit().dialog.set(*params);
}

int evo_hui_render_dialog(int width, int height)
{
    Kit &k = kit();
    return k.render_overlay(Screen::dialog, width, height,
                            [&](float dt, hui::gfx::DrawList &list, const Context &ctx) {
                                k.dialog.update(dt);
                                k.dialog.draw(list, ctx);
                            });
}

void evo_hui_update_toast(const evo_rmlui_toast_params_t *params)
{
    if (params)
        kit().toast.set(*params);
}

int evo_hui_render_toast(int width, int height)
{
    Kit &k = kit();
    return k.render_overlay(Screen::toast, width, height,
                            [&](float dt, hui::gfx::DrawList &list, const Context &ctx) {
                                k.toast.update(dt);
                                k.toast.draw(list, ctx);
                            });
}

void evo_hui_update_twitch_home(const evo_hui_twitch_home_params_t *params)
{
    if (params)
        kit().twitch_home.set(*params);
}

int evo_hui_render_twitch_home(int width, int height)
{
    Kit &k = kit();
    return k.render(Screen::twitch_home, width, height,
                    [&](float dt, hui::gfx::DrawList &list, const Context &ctx) {
                        k.twitch_home.update(dt);
                        k.twitch_home.draw(list, ctx);
                    });
}

void evo_hui_update_twitch_browse(const evo_hui_twitch_browse_params_t *params)
{
    if (params)
        kit().twitch_browse.set(*params);
}

int evo_hui_render_twitch_browse(int width, int height)
{
    Kit &k = kit();
    return k.render(Screen::twitch_browse, width, height,
                    [&](float dt, hui::gfx::DrawList &list, const Context &ctx) {
                        k.twitch_browse.update(dt);
                        k.twitch_browse.draw(list, ctx);
                    });
}

void evo_hui_update_twitch_category(const evo_hui_twitch_category_params_t *params)
{
    if (params)
        kit().twitch_category.set(*params);
}

int evo_hui_render_twitch_category(int width, int height)
{
    Kit &k = kit();
    return k.render(Screen::twitch_category, width, height,
                    [&](float dt, hui::gfx::DrawList &list, const Context &ctx) {
                        k.twitch_category.update(dt);
                        k.twitch_category.draw(list, ctx);
                    });
}

void evo_hui_update_twitch_search(const evo_hui_twitch_search_params_t *params)
{
    if (params)
        kit().twitch_search.set(*params);
}

int evo_hui_render_twitch_search(int width, int height)
{
    Kit &k = kit();
    return k.render(Screen::twitch_search, width, height,
                    [&](float dt, hui::gfx::DrawList &list, const Context &ctx) {
                        k.twitch_search.update(dt);
                        k.twitch_search.draw(list, ctx);
                    });
}

void evo_hui_update_twitch_channel(const evo_hui_twitch_channel_params_t *params)
{
    if (params)
        kit().twitch_channel.set(*params);
}

int evo_hui_render_twitch_channel(int width, int height)
{
    Kit &k = kit();
    return k.render(Screen::twitch_channel, width, height,
                    [&](float dt, hui::gfx::DrawList &list, const Context &ctx) {
                        k.twitch_channel.update(dt);
                        k.twitch_channel.draw(list, ctx);
                    });
}

void evo_hui_update_twitch_following(const evo_hui_twitch_following_params_t *params)
{
    if (params)
        kit().twitch_following.set(*params);
}

int evo_hui_render_twitch_following(int width, int height)
{
    Kit &k = kit();
    return k.render(Screen::twitch_following, width, height,
                    [&](float dt, hui::gfx::DrawList &list, const Context &ctx) {
                        k.twitch_following.update(dt);
                        k.twitch_following.draw(list, ctx);
                    });
}

void evo_hui_update_twitch_login(const evo_hui_twitch_login_params_t *params)
{
    if (params)
        kit().twitch_login.set(*params);
}

int evo_hui_render_twitch_login(int width, int height)
{
    Kit &k = kit();
    /* Full screen: the rail is hidden here (ScreenManager gives the login
     * screen no section), so it draws nothing anyway. */
    return k.render(Screen::twitch_login, width, height,
                    [&](float dt, hui::gfx::DrawList &list, const Context &ctx) {
                        k.twitch_login.update(dt);
                        k.twitch_login.draw(list, ctx);
                    },
                    false);
}

void evo_hui_twitch_chat_update(const evo_hui_twitch_chat_params_t *p)
{
    if (p)
        kit().twitch_chat.set(*p);
}

int evo_hui_render_twitch_chat(int width, int height)
{
    Kit &k = kit();
    /* Over the video, as the OSD is: no rail, no backdrop, no clear. Before
     * or after the OSD's render, which keeps charge of the frame. */
    return k.render(
        Screen::twitch_chat, width, height,
        [&](float dt, hui::gfx::DrawList &list, const Context &ctx) {
            k.twitch_chat.update(dt);
            k.twitch_chat.draw(list, ctx);
        },
        false);
}

void evo_hui_twitch_clip_update(const evo_hui_twitch_clip_params_t *p)
{
    if (p)
        kit().twitch_clip.set(*p);
}

int evo_hui_render_twitch_clip(int width, int height)
{
    Kit &k = kit();
    /* An overlay: the OSD (or chat) keeps charge of the frame. */
    return k.render_overlay(Screen::twitch_clip, width, height,
                            [&](float dt, hui::gfx::DrawList &list, const Context &ctx) {
                                k.twitch_clip.update(dt);
                                k.twitch_clip.draw(list, ctx);
                            });
}

void evo_hui_update_keyboard(const evo_keyboard_params_t *params)
{
    if (!params)
        return;
    Kit &k = kit();
    k.keyboard_visible = params->visible != 0;
    k.keyboard.set(*params);
}

int evo_hui_render_keyboard(int width, int height)
{
    Kit &k = kit();
    if (!k.keyboard_visible)
        return 0; /* nothing to draw: RmlUi's keyboard is hidden too */
    return k.render_overlay(Screen::keyboard, width, height,
                            [&](float dt, hui::gfx::DrawList &list, const Context &ctx) {
                                k.keyboard.update(dt);
                                k.keyboard.draw(list, ctx);
                            });
}

} /* extern "C" */
