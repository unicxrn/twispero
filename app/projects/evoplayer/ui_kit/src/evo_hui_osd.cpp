/*
 * evo_hui_osd.cpp - the player OSD on ps5-homebrew-ui. See the header.
 */
#include "evo_hui_osd.hpp"

#include "ui/glyphs.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace evo::kit
{

using ::hui::gfx::Align;
using ::hui::gfx::Color;
using ::hui::gfx::DrawList;
using ::hui::gfx::Rect;
namespace tween = ::hui::tween;
namespace ui = ::hui::ui;

namespace
{

const Color kWhite = Color::rgb(0xffffff);
const Color kBlack = Color::rgb(0x000000);
const Color kClear = Color::rgb(0x000000, 0.0f);
const Color kCyan = Color::rgb(0x4dd8ff);
const Color kAmber = Color::rgb(0xffb547);
const Color kGreen = Color::rgb(0x5fe39a);

constexpr float kMargin = 96.0f;
constexpr float kRight = 1920.0f - kMargin;

/* The channel block at the top left: the avatar, then the name, the stream
 * title (two lines at most) and "Category · 12.3K watching" beside it. */
constexpr float kAvatarR = 56.0f;
constexpr float kAvatarCx = kMargin + kAvatarR;
constexpr float kAvatarCy = 64.0f + kAvatarR;
constexpr float kTextX = kMargin + 2.0f * kAvatarR + 32.0f;
constexpr float kTextW = 1080.0f;
constexpr float kNameSize = 36.0f;
constexpr float kNameY = 98.0f;
constexpr float kTitleSize = 26.0f;
constexpr float kTitleFirst = 42.0f; /* name baseline to the title's */
constexpr float kTitleLead = 35.0f;
constexpr float kMetaSize = 24.0f;
constexpr float kMetaGap = 42.0f;    /* the last line's baseline to the category's */

/* The quality pill at the top right, centred on the name's capitals. */
constexpr float kPillH = 38.0f;
constexpr float kPillY = 66.0f;
constexpr float kPillText = 20.0f;

/* The audio-only view: the avatar in the middle of the screen. */
constexpr float kBigR = 116.0f;
constexpr float kBigCy = 360.0f;
constexpr float kBigTextW = 1100.0f;

/* Where the stats panel starts: under the channel block, or the title. */
constexpr float kStatsTwitchY = 280.0f;
constexpr float kStatsY = 200.0f;

std::string fit(const ui::FontRef &font, const std::string &text, float size, float width)
{
    return font.font->fit(text, size, width);
}

std::string clock_text(double seconds)
{
    if (!(seconds >= 0.0))
        seconds = 0.0;
    const long s = static_cast<long>(seconds);
    char b[32];
    if (s >= 3600)
        std::snprintf(b, sizeof b, "%ld:%02ld:%02ld", s / 3600, (s / 60) % 60, s % 60);
    else
        std::snprintf(b, sizeof b, "%ld:%02ld", s / 60, s % 60);
    return b;
}

/* The first character of text, upper case when it is a plain letter. */
std::string first_letter(const std::string &text)
{
    if (text.empty())
        return std::string();
    std::size_t n = 1;
    const unsigned char c = static_cast<unsigned char>(text[0]);
    if (c >= 0xF0)
        n = 4;
    else if (c >= 0xE0)
        n = 3;
    else if (c >= 0xC0)
        n = 2;
    std::string out = text.substr(0, std::min(n, text.size()));
    if (out.size() == 1 && out[0] >= 'a' && out[0] <= 'z')
        out[0] = static_cast<char>(out[0] - 'a' + 'A');
    return out;
}

/* draw_live_pill's width at this size (the same sums). */
float live_pill_width(const Context &ctx, float size)
{
    return ctx.fonts.semibold.measure("LIVE", size, size * 0.08f) + 2.0f * size * 0.55f;
}

const char *chat_hint(int mode)
{
    switch (mode)
    {
    /* The words Settings and R1's toast use. */
    case EVO_OSD_CHAT_SIDE:
        return "Chat: Next to the video";
    case EVO_OSD_CHAT_OVERLAY:
        return "Chat: Over the video";
    default:
        return "Chat: Off";
    }
}

} // namespace

void OsdScreen::set(const evo_playback_osd_params_t &p)
{
    auto take = [this](std::string &dst, const char *src) {
        const char *v = src ? src : "";
        if (dst != v)
        {
            dst = v;
            ++rev_;
        }
    };
    if ((raw_.twitch != 0) != (p.twitch != 0) || (raw_.twitch_video != 0) != (p.twitch_video != 0))
        ++rev_; /* the name (or the line under the title) comes from another field */
    raw_ = p;
    take(title_, p.title);
    take(meta_, p.metadata);
    take(status_, p.status);
    take(quality_, p.quality);
    take(channel_, p.channel);
    take(login_, p.channel_login);
    take(stream_title_, p.stream_title);
    take(category_, p.category);
    take(video_meta_, p.video_meta);
    if (viewers_ != p.viewers)
    {
        viewers_ = p.viewers;
        ++rev_;
    }
    evo_hui_twitch_image_t avatar{};
    avatar.rgba = p.avatar_rgba;
    avatar.w = p.avatar_w;
    avatar.h = p.avatar_h;
    avatar.id = p.avatar_id;
    avatar_ = twitch_picture(avatar);
    avatar_motion_.track(avatar_);
    /* Never read the copied pointers; the avatar's pixels are avatar_'s. */
    raw_.title = raw_.metadata = raw_.status = raw_.quality = nullptr;
    raw_.channel = raw_.channel_login = raw_.stream_title = raw_.category = raw_.video_meta = nullptr;
    raw_.avatar_rgba = nullptr;
    hud_on_ = p.show_stats != 0;
}

void OsdScreen::set_hud(const evo_perf_hud_t &h)
{
    auto str = [](const char *s) { return std::string(s ? s : ""); };
    hud_lines_[0] = str(h.line_video);
    hud_lines_[1] = str(h.line_audio);
    hud_lines_[2] = str(h.line_perf);
    hud_lines_[3] = str(h.line_queues);
    hud_lines_[4] = str(h.line_clocks);
    auto hist = [&](const float *src, std::vector<float> &dst) {
        dst.clear();
        for (int i = 0; src && i < h.hist_len; ++i)
            dst.push_back(src[i]);
    };
    hist(h.gpu_hist, gpu_hist_);
    hist(h.ram_hist, ram_hist_);
    hist(h.cpu_hist, cpu_hist_);
    gpu_pct_ = h.gpu_pct;
    ram_mb_ = h.ram_mb;
    ram_total_ = h.ram_total_mb;
    cpu_pct_ = h.cpu_pct;
}

void OsdScreen::enter()
{
    progress_snapped_ = false;
    chrome_.snap(0.0f);
}

void OsdScreen::update(float dt)
{
    time_ += dt;
    /* The caller already fades `alpha`; the spring only smooths its steps. */
    chrome_.target = static_cast<float>(raw_.alpha) / 255.0f;
    chrome_.update(dt, 20.0f);
    paused_.target = (raw_.paused && !raw_.scrub_active && !raw_.loading) ? 1.0f : 0.0f;
    paused_.update(dt, 16.0f);
    loading_.target = (raw_.loading || !status_.empty()) && !raw_.scrub_active ? 1.0f : 0.0f;
    loading_.update(dt, 14.0f);
    scrub_.target = raw_.scrub_active ? 1.0f : 0.0f;
    scrub_.update(dt, 18.0f);
    avatar_motion_.step(dt, false, false);
    const float pct = static_cast<float>(std::clamp(raw_.percentage > 1.0 ? raw_.percentage / 100.0
                                                                           : raw_.percentage,
                                                    0.0, 1.0));
    if (!progress_snapped_)
    {
        progress_.snap(pct);
        progress_snapped_ = true;
    }
    progress_.target = pct;
    progress_.update(dt, 14.0f);
}

const OsdScreen::Lines &OsdScreen::lines(const Context &ctx) const
{
    const ui::Fonts &fonts = ctx.fonts;
    Lines &l = lines_;
    if (l.rev == rev_ && l.fonts == &fonts)
        return l;
    l.rev = rev_;
    l.fonts = &fonts;

    /* The name in the face it is drawn in; the login when nothing of it is
     * left (a name all in a script the fonts lack). */
    std::string name = drawable(fonts.semibold, raw_.twitch ? channel_ : title_);
    if (name.empty())
        name = raw_.twitch ? login_ : title_;
    l.initial = first_letter(name);
    l.name = fit(fonts.semibold, name, kNameSize, kTextW);
    l.big_name = fit(fonts.semibold, name, 56, 1400);

    const std::string title = drawable(fonts.regular, raw_.twitch ? stream_title_ : meta_);
    l.title = fit_lines(fonts.regular, title, kTitleSize, kTextW, 2);
    l.big_title = fit_lines(fonts.regular, title, 28, kBigTextW, 2);

    l.category = fit(fonts.semibold, drawable(fonts.semibold, category_), kMetaSize, kTextW * 0.6f);
    if (raw_.twitch_video)
        l.watching = fit(fonts.regular, drawable(fonts.regular, video_meta_), kMetaSize, kTextW * 0.6f);
    else
        l.watching = viewers_ >= 0 ? format_viewers(viewers_) + " watching" : std::string();

    /* Through drawable() like every other line here: a character the font
     * lacks would otherwise come out as a box. */
    l.vod_title = fit(fonts.display, drawable(fonts.display, title_), 44, 1500);
    l.vod_meta = fit(fonts.regular, drawable(fonts.regular, meta_), 22, 1500);
    return l;
}

/* ------------------------------------------------------------------ the top */

void OsdScreen::draw_avatar(DrawList &list, const Context &ctx, float cx, float cy, float radius, float pill) const
{
    const ui::Fonts &fonts = ctx.fonts;
    const Lines &l = lines(ctx);
    const Rect box{cx - radius, cy - radius, 2.0f * radius, 2.0f * radius};
    list.shadow({box.x, box.y + radius * 0.12f, box.w, box.h}, radius, radius * 0.45f, kBlack.with_alpha(0.45f));

    /* Until the picture is in: the first letter on Twitch purple. */
    const std::uint32_t tex = twitch_texture(ctx, avatar_);
    const float art_in = tex ? avatar_motion_.art_in : 0.0f;
    if (art_in < 1.0f)
    {
        list.circle(cx, cy, radius, ctx.palette.accent);
        if (!l.initial.empty())
            ui::text(list, fonts.semibold, l.initial, cx, cy + radius * 0.34f, radius * 0.95f, kWhite, Align::center);
    }
    if (tex)
        list.image(tex, box, cover_uv(avatar_.w, avatar_.h, box.w, box.h), kWhite.with_alpha(art_in), radius);

    /* A video has no red ring, even one with no length yet (a broadcast that
     * is still being recorded plays as a live stream). */
    if (!raw_.is_live || raw_.twitch_video)
        return;
    /* Live: Twitch's red ring, clear of the picture, and the tag on it. */
    const float gap = std::max(3.0f, radius * 0.05f);
    const float stroke = std::max(3.0f, radius * 0.04f);
    list.ring(cx, cy, radius + gap + stroke, stroke, twitch_live_red());
    const float w = live_pill_width(ctx, pill);
    const float h = std::round(pill * 1.6f);
    draw_live_pill(list, ctx, cx - w * 0.5f, cy + radius + gap + stroke * 0.5f - h * 0.5f, pill);
}

void OsdScreen::draw_channel(DrawList &list, const Context &ctx) const
{
    const ui::Fonts &fonts = ctx.fonts;
    const Lines &l = lines(ctx);

    draw_avatar(list, ctx, kAvatarCx, kAvatarCy, kAvatarR, 15.0f);

    ui::text(list, fonts.semibold, l.name, kTextX, kNameY, kNameSize, kWhite);
    float y = kNameY; /* the last baseline drawn */
    for (std::size_t i = 0; i < l.title.size(); ++i)
    {
        y += i == 0 ? kTitleFirst : kTitleLead;
        ui::text(list, fonts.regular, l.title[i], kTextX, y, kTitleSize, kWhite.with_alpha(0.92f));
    }

    if (!l.category.empty() || !l.watching.empty())
        draw_meta(list, ctx, kTextX, y + kMetaGap, false);
}

void OsdScreen::draw_meta(DrawList &list, const Context &ctx, float x, float y, bool centred) const
{
    /* "Category · 12.3K watching": the category in Twitch's link purple. */
    const ui::Fonts &fonts = ctx.fonts;
    const Lines &l = lines(ctx);
    const char *dot = " \xC2\xB7 ";
    const bool both = !l.category.empty() && !l.watching.empty();
    if (centred)
    {
        float w = fonts.semibold.measure(l.category, kMetaSize) + fonts.regular.measure(l.watching, kMetaSize);
        if (both)
            w += fonts.regular.measure(dot, kMetaSize);
        x -= w * 0.5f;
    }
    if (!l.category.empty())
        x += ui::text(list, fonts.semibold, l.category, x, y, kMetaSize, ctx.palette.accent_alt);
    if (both)
        x += ui::text(list, fonts.regular, dot, x, y, kMetaSize, kWhite.with_alpha(0.6f));
    if (!l.watching.empty())
        ui::text(list, fonts.regular, l.watching, x, y, kMetaSize, kWhite.with_alpha(0.78f));
}

void OsdScreen::draw_title(DrawList &list, const Context &ctx) const
{
    const ui::Fonts &fonts = ctx.fonts;
    const Lines &l = lines(ctx);
    /* No title left to draw (all Japanese, say): the line under it moves up
     * into its place rather than sitting under a blank one. */
    float meta_y = 110;
    if (!l.vod_title.empty())
    {
        ui::text(list, fonts.display, l.vod_title, kMargin, 110, 44, kWhite);
        meta_y = 148;
    }
    if (!l.vod_meta.empty())
        ui::text(list, fonts.regular, l.vod_meta, kMargin, meta_y, 22, kWhite.with_alpha(0.7f));
}

void OsdScreen::draw_quality(DrawList &list, const Context &ctx) const
{
    if (quality_.empty())
        return;
    const ui::Fonts &fonts = ctx.fonts;
    const float w = fonts.semibold.measure(quality_, kPillText, 0.4f) + 30.0f;
    const Rect r{kRight - w, kPillY, w, kPillH};
    list.bordered_rect(r, 8, kBlack.with_alpha(0.42f), 1.5f, kWhite.with_alpha(0.32f));
    ui::text(list, fonts.semibold, quality_, r.cx(), r.y + 26, kPillText, kWhite, Align::center, 0.4f);
}

/* --------------------------------------------------------------- the bottom */

void OsdScreen::draw_bottom(DrawList &list, const Context &ctx) const
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    /* A Twitch video or clip has a file's timeline under its channel. */
    const bool timeline = !raw_.is_live && (!raw_.twitch || raw_.twitch_video);
    /* The shade ramps up to near solid by the keys' row, so the keys read
     * over a busy picture too (a scoreboard or facecam in a real stream
     * showed through the old, lighter one). */
    const float shade_top = timeline ? 740.0f : 820.0f;
    list.gradient_rect({0, shade_top, 1920, 980 - shade_top}, 0, kBlack.with_alpha(0.0f), kBlack.with_alpha(0.8f));
    list.gradient_rect({0, 980, 1920, 100}, 0, kBlack.with_alpha(0.8f), kBlack.with_alpha(0.86f));

    if (timeline)
    {
        const Rect track{kMargin, 920, 1920 - 2 * kMargin, 8};
        const float f = progress_.value;
        list.rounded_rect(track, 4, kWhite.with_alpha(0.22f));
        list.rounded_rect({track.x, track.y, std::max(8.0f, track.w * f), track.h}, 4, p.accent);
        const float tx = track.x + track.w * f;
        list.glow({tx - 9, track.cy() - 9, 18, 18}, 9, 10, p.accent.with_alpha(0.5f));
        list.circle(tx, track.cy(), 9, kWhite);
        if (scrub_.value > 0.01f && raw_.duration_sec > 0.0)
        {
            /* Where the seek will land, ahead of (or behind) the playhead. */
            const float t = static_cast<float>(std::clamp(raw_.scrub_target / raw_.duration_sec, 0.0, 1.0));
            const float sx = track.x + track.w * t;
            list.push_opacity(scrub_.value);
            list.rounded_rect({sx - 2, track.y - 14, 4, track.h + 28}, 2, kWhite);
            list.pop_opacity();
        }
        /* The times in the UI's own type, not the stats panel's mono. */
        ui::text(list, fonts.semibold, clock_text(raw_.position_sec), track.x, 966, 24, kWhite);
        ui::text(list, fonts.semibold, clock_text(raw_.duration_sec), track.x + track.w, 966, 24,
                 kWhite.with_alpha(0.7f), Align::right);
    }

    /* The keys, one row of hints at the bottom right. */
    ui::Hint hints[8];
    int n = 0;
    char aspect[32];
    if (raw_.twitch && raw_.twitch_video)
    {
        /* No chat to change or write to; a timeline to move along. */
        hints[n++] = {ui::Button::cross, raw_.paused ? "Play" : "Pause"};
        /* A broadcast still being recorded plays like a live stream: no
         * seeking, and its quality stays (evo_twitch.h). */
        if (!raw_.is_live)
            hints[n++] = {ui::Button::dpad, "Skip 10 s"};
        if (!quality_.empty() && !raw_.is_live)
            hints[n++] = {ui::Button::r2, "Quality"};
    }
    else if (raw_.twitch)
    {
        /* Cross pauses a stream too (PlayerScreen), as on a video. */
        hints[n++] = {ui::Button::cross, raw_.paused ? "Play" : "Pause"};
        if (!quality_.empty())
            hints[n++] = {ui::Button::r2, "Quality"};
        hints[n++] = {ui::Button::r1, chat_hint(raw_.chat_mode)};
        if (raw_.can_chat)
            hints[n++] = {ui::Button::triangle, "Send message"};
        if (raw_.can_chat && raw_.chat_mode != EVO_OSD_CHAT_OFF)
            hints[n++] = {ui::Button::l1, "Reply"};
        if (raw_.can_clip)
            hints[n++] = {ui::Button::square, "Clip"};
    }
    else
    {
        std::snprintf(aspect, sizeof aspect, "Aspect: %s",
                      raw_.view_mode == 0 ? "Fit" : (raw_.view_mode == 1 ? "Fill" : "Stretch"));
        hints[n++] = {ui::Button::cross, raw_.paused ? "Play" : "Pause"};
        if (!raw_.is_live)
            hints[n++] = {ui::Button::dpad, "Skip 10 s"};
        hints[n++] = {ui::Button::triangle, aspect};
    }
    /* Dev builds only: a release build has no stats panel (PlayerScreen). */
    if (raw_.can_stats)
        hints[n++] = {ui::Button::options, "Stats"};
    if (!(raw_.twitch && raw_.clip_card))
        hints[n++] = {ui::Button::circle, "Back"};
    ui::HintLayout layout;
    layout.size = 32;
    layout.text_size = 22;
    layout.cy = 1018;
    layout.item_gap = 40;
    ui::draw_hints(list, fonts, ui::GlyphStyle::dark(), hints, n, kRight, true, layout);
}

/* --------------------------------------------------------------- the centre */

void OsdScreen::draw_centre(DrawList &list, const Context &ctx) const
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    if (loading_.value > 0.01f)
    {
        list.push_opacity(loading_.value);
        list.circle(960, 520, 64, kBlack.with_alpha(0.55f));
        list.arc(960, 520, 44, 6, 0.0f, 6.2831853f, kWhite.with_alpha(0.15f));
        list.arc(960, 520, 44, 6, std::fmod(time_ * 5.5f, 6.2831853f), 1.6f, p.accent);
        ui::text(list, fonts.semibold, status_.empty() ? std::string("LOADING") : ui::upper(status_), 960, 630, 18,
                 kWhite.with_alpha(0.85f), Align::center, 3.0f);
        list.pop_opacity();
    }
    if (paused_.value > 0.01f)
    {
        const float s = 0.85f + 0.15f * paused_.value;
        list.push_transform(s, 960, 520, 0, 0);
        list.push_opacity(paused_.value);
        list.circle(960, 520, 70, kBlack.with_alpha(0.55f));
        list.rounded_rect({960 - 22, 520 - 28, 14, 56}, 4, kWhite);
        list.rounded_rect({960 + 8, 520 - 28, 14, 56}, 4, kWhite);
        ui::text(list, fonts.semibold, "PAUSED", 960, 636, 18, kWhite.with_alpha(0.85f), Align::center, 3.0f);
        list.pop_opacity();
        list.pop_transform();
    }
    if (scrub_.value > 0.01f)
    {
        list.push_opacity(scrub_.value);
        const Rect cap{960 - 170, 470, 340, 110};
        list.rounded_rect(cap, 28, kBlack.with_alpha(0.6f));
        ui::text(list, fonts.semibold, "JUMP TO", 960, cap.y + 38, 16, p.accent, Align::center, 3.0f);
        ui::text(list, fonts.semibold, clock_text(raw_.scrub_target), 960, cap.y + 86, 40, kWhite, Align::center);
        list.pop_opacity();
    }
}

/* ----------------------------------------------------------- audio only */

void OsdScreen::draw_audio_only(DrawList &list, const Context &ctx) const
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    const Lines &l = lines(ctx);

    /* No picture to draw over: the backdrop, washed with the avatar's colours. */
    const Rect screen{0, 0, 1920, 1080};
    list.gradient_rect(screen, 0, p.bg_top, p.bg_bottom);
    const std::uint32_t tex = twitch_texture(ctx, avatar_);
    if (tex)
    {
        list.image(tex, screen, cover_uv(avatar_.w, avatar_.h, screen.w, screen.h),
                   kWhite.with_alpha(0.32f * avatar_motion_.art_in));
        list.gradient_rect(screen, 0, kBlack.with_alpha(0.55f), kBlack.with_alpha(0.88f));
    }
    list.shadow({960 - 420, kBigCy - 300, 840, 600}, 300, 260, p.accent.with_alpha(0.16f));

    draw_avatar(list, ctx, 960, kBigCy, kBigR, 20.0f);

    float y = kBigCy + kBigR + 106.0f;
    ui::text(list, fonts.semibold, l.big_name, 960, y, 56, kWhite, Align::center);
    for (std::size_t i = 0; i < l.big_title.size(); ++i)
    {
        y += i == 0 ? 50.0f : 38.0f;
        ui::text(list, fonts.regular, l.big_title[i], 960, y, 28, kWhite.with_alpha(0.86f), Align::center);
    }
    if (!l.category.empty() || !l.watching.empty())
    {
        y += 46.0f;
        draw_meta(list, ctx, 960, y, true);
    }

    /* "Audio only", with a small level meter that moves while it plays. */
    const std::string chip = !status_.empty() ? status_ : (raw_.paused ? std::string("Paused") : "Audio only");
    const float tw = fonts.semibold.measure(chip, 22);
    const float bars = 22.0f;
    const float cw = 24.0f + bars + 12.0f + tw + 24.0f;
    const Rect c{960 - cw * 0.5f, y + 38.0f, cw, 48};
    list.rounded_rect(c, 24, kWhite.with_alpha(0.1f));
    list.bordered_rect(c, 24, kClear, 1.5f, kWhite.with_alpha(0.14f));
    for (int i = 0; i < 3; ++i)
    {
        const float ph = time_ * (raw_.paused ? 0.0f : 7.0f) + static_cast<float>(i) * 1.9f;
        const float h = raw_.paused ? 6.0f : 7.0f + 11.0f * (0.5f + 0.5f * std::sin(ph));
        list.rounded_rect({c.x + 24 + static_cast<float>(i) * 8.0f, c.cy() + 9.0f - h, 5, h}, 2.5f, p.accent_alt);
    }
    ui::text(list, fonts.semibold, chip, c.x + 24 + bars + 12, c.y + 32, 22, kWhite.with_alpha(0.92f));
}

/* --------------------------------------------------------------- the stats */

void OsdScreen::draw_stats(DrawList &list, const Context &ctx) const
{
    const ui::Fonts &fonts = ctx.fonts;
    const Rect panel{kMargin, raw_.twitch ? kStatsTwitchY : kStatsY, 760, 540};
    list.rounded_rect(panel, 24, kBlack.with_alpha(0.72f));
    list.bordered_rect(panel, 24, kClear, 1.0f, kWhite.with_alpha(0.12f));
    ui::text(list, fonts.semibold, "PLAYBACK DIAGNOSTICS", panel.x + 28, panel.y + 44, 16, ctx.palette.accent,
             Align::left, 3.0f);
    float y = panel.y + 84;
    for (const std::string &l : hud_lines_)
    {
        if (l.empty())
            continue;
        ui::text(list, fonts.mono, fit(fonts.mono, l, 17, panel.w - 56), panel.x + 28, y, 17, kWhite.with_alpha(0.9f));
        y += 30;
    }
    struct Graph
    {
        const char *label;
        const std::vector<float> *hist;
        Color color;
        char value[32];
    };
    Graph graphs[3] = {{"GPU", &gpu_hist_, kCyan, {}}, {"RAM", &ram_hist_, kAmber, {}}, {"CPU", &cpu_hist_, kGreen, {}}};
    std::snprintf(graphs[0].value, sizeof graphs[0].value, "%.0f%%", static_cast<double>(gpu_pct_));
    std::snprintf(graphs[1].value, sizeof graphs[1].value, "%.0f MB", static_cast<double>(ram_mb_));
    std::snprintf(graphs[2].value, sizeof graphs[2].value, "%.0f%%", static_cast<double>(cpu_pct_));
    float gx = panel.x + 28;
    const float gy = panel.y + panel.h - 130;
    for (const Graph &g : graphs)
    {
        const Rect box{gx, gy, 220, 100};
        list.rounded_rect(box, 12, kWhite.with_alpha(0.05f));
        ui::text(list, fonts.semibold, g.label, box.x + 12, box.y + 24, 14, g.color, Align::left, 2.0f);
        ui::text(list, fonts.mono, g.value, box.x + box.w - 12, box.y + 24, 14, kWhite, Align::right);
        const std::vector<float> &h = *g.hist;
        if (h.size() > 1)
        {
            const float step = (box.w - 24) / static_cast<float>(h.size() - 1);
            for (std::size_t i = 1; i < h.size(); ++i)
            {
                const float x0 = box.x + 12 + step * static_cast<float>(i - 1);
                const float y0 = box.y + box.h - 10 - 56 * std::clamp(h[i - 1], 0.0f, 1.0f);
                const float x1 = box.x + 12 + step * static_cast<float>(i);
                const float y1 = box.y + box.h - 10 - 56 * std::clamp(h[i], 0.0f, 1.0f);
                list.line(x0, y0, x1, y1, 2.5f, g.color);
            }
        }
        gx += 236;
    }
}

void OsdScreen::draw(DrawList &list, const Context &ctx) const
{
    const ui::Fonts &fonts = ctx.fonts;
    const bool audio_only = raw_.music_mode != 0;
    if (audio_only)
        draw_audio_only(list, ctx);
    const float chrome = chrome_.value;
    if (chrome > 0.01f)
    {
        list.push_opacity(chrome);
        if (!audio_only)
        {
            list.gradient_rect({0, 0, 1920, 300}, 0, kBlack.with_alpha(0.78f), kBlack.with_alpha(0.0f));
            if (raw_.twitch)
            {
                draw_channel(list, ctx);
                draw_quality(list, ctx);
            }
            else
            {
                draw_title(list, ctx);
            }
        }
        draw_bottom(list, ctx);
        list.pop_opacity();
    }
    if (!audio_only)
        draw_centre(list, ctx);
    if (hud_on_)
        draw_stats(list, ctx);
    if (raw_.debug_overlay)
    {
        char fps[24];
        std::snprintf(fps, sizeof fps, "%d FPS", raw_.fps);
        const Rect chip{kMargin, 996, 140, 40};
        list.rounded_rect(chip, 20, kBlack.with_alpha(0.6f));
        ui::text(list, fonts.mono, fps, chip.cx(), chip.y + 28, 20, ctx.palette.accent, Align::center);
    }
}

bool OsdScreen::focus(FocusInfo *out) const
{
    /* The player has no cursor; report the state the remote tests look for. */
    out->focusable.clear();
    out->id = raw_.scrub_active ? "osd-scrub" : (raw_.paused ? "osd-paused" : "osd-playing");
    out->text = title_;
    out->index = out->total = 1;
    out->rect = {0, 0, 1920, 1080};
    return chrome_.value > 0.01f || raw_.scrub_active || raw_.paused;
}

} // namespace evo::kit
