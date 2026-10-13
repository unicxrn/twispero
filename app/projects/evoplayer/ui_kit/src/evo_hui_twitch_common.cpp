/*
 * evo_hui_twitch_common.cpp - pieces the Twitch screens share. See
 * evo_hui_twitch.hpp.
 */
#include "evo_hui_twitch.hpp"

#include "ui/components/progress.hpp"
#include "ui/glyphs.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

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
const Color kClear = Color::rgb(0x000000, 0.0f);
constexpr float kLeft = tw::kLeft;
constexpr float kRight = tw::kRight;
constexpr float kRadius = tw::kRadius;

std::string str(const char *s)
{
    return std::string(s ? s : "");
}

std::string fit(const ui::FontRef &font, const std::string &text, float size, float width)
{
    return font.font->fit(text, size, width);
}
} // namespace

/* ------------------------------------------------------------------ the mark */

void draw_app_mark(DrawList &list, const Context &ctx, float x, float y, float size)
{
    /* The tiles are 256 and 128 px with their corners already round (at 22 %
     * of the side, gen_brand.py's TILE_RADIUS). The smaller one serves the
     * rail and the login header, so neither is shrunk much more than twice. */
    const Rect r{x, y, size, size};
    const char *path = size <= 80.0f ? "../icons/icon_app_small.png" : "../icons/icon_app.png";
    if (const std::uint32_t tex = ctx.textures.icon(path))
        list.image(tex, r, ::hui::gfx::kFullUv, kWhite);
    else
        list.rounded_rect(r, size * 0.22f, Color::rgb(0x8a44f5)); /* the tile's purple */
}

void draw_app_lettering(DrawList &list, const Context &ctx, float x, float y, float width, Color color)
{
    /* White letters on a 288 x 144 canvas: the tint is their colour. */
    if (const std::uint32_t tex = ctx.textures.icon("../icons/icon_app_mono.png"))
        list.image(tex, {x, y, width, width * 0.5f}, ::hui::gfx::kFullUv, color);
}

/* --------------------------------------------------------------- skeletons */

void draw_skeleton(DrawList &list, const Context &ctx, const Rect &r, float radius, float alpha)
{
    if (r.w < 1.0f || r.h < 1.0f || alpha <= 0.0f)
        return;
    const Palette &p = ctx.palette;
    list.rounded_rect(r, radius, p.surface_sel.with_alpha(p.surface_sel.a * alpha));
    /* One soft band crosses the whole screen every 2.2 s, left to right, so
     * the blocks light up in order and read as one surface loading. */
    constexpr float kPeriod = 2.2f, kSweep = 1.6f, kHalf = 260.0f;
    const float t = std::fmod(ctx.time, kPeriod) / kSweep;
    if (t >= 1.0f)
        return;
    const float centre = tween::lerp(-kHalf, 1920.0f + kHalf, tween::smoothstep(t));
    ::hui::ui::draw_sweep(list, centre, kHalf, r.x + radius * 0.5f, r.x + r.w - radius * 0.5f, r.y, r.h,
                          Color::rgb(0xffffff, 0.05f * alpha));
}

/* ------------------------------------------------------------------ pieces */

std::string format_viewers(int viewers)
{
    char b[24];
    const long long v = viewers < 0 ? 0 : viewers;
    /* Cut, never round up: 999,999 is "999K", not "1000K". */
    auto one_decimal = [&](long long unit, const char *suffix) {
        const long long tenths = v * 10 / unit;
        if (tenths % 10 == 0)
            std::snprintf(b, sizeof b, "%lld%s", tenths / 10, suffix);
        else
            std::snprintf(b, sizeof b, "%lld.%lld%s", tenths / 10, tenths % 10, suffix);
    };
    if (v < 1000)
        std::snprintf(b, sizeof b, "%lld", v);
    else if (v < 100000)
        one_decimal(1000, "K");
    else if (v < 1000000)
        std::snprintf(b, sizeof b, "%lldK", v / 1000);
    else
        one_decimal(1000000, "M");
    return b;
}

Rect cover_uv(int w, int h, float box_w, float box_h)
{
    if (w <= 0 || h <= 0 || box_w <= 0.0f || box_h <= 0.0f)
        return ::hui::gfx::kFullUv;
    const float img = static_cast<float>(w) / static_cast<float>(h);
    const float box = box_w / box_h;
    if (img > box)
    {
        const float f = box / img;
        return {(1.0f - f) * 0.5f, 0.0f, f, 1.0f};
    }
    const float f = img / box;
    return {0.0f, (1.0f - f) * 0.5f, 1.0f, f};
}

float draw_live_pill(DrawList &list, const Context &ctx, float x, float y, float size)
{
    const ui::FontRef &font = ctx.fonts.semibold;
    const float tracking = size * 0.08f;
    const float pad = size * 0.55f;
    const float h = std::round(size * 1.6f);
    const float w = font.measure("LIVE", size, tracking) + 2.0f * pad;
    list.rounded_rect({x, y, w, h}, size * 0.3f, twitch_live_red());
    /* Optical centre: the caps sit a touch above the box's middle. */
    ui::text(list, font, "LIVE", x + w * 0.5f, y + h * 0.5f + size * 0.36f, size, Color::rgb(0xffffff),
             Align::center, tracking);
    return w;
}

void draw_spinner(DrawList &list, const Context &ctx, float cx, float cy, float radius, Color color)
{
    const float t = ctx.time;
    list.ring(cx, cy, radius, radius * 0.18f, color.with_alpha(color.a * 0.18f));
    const float sweep = 1.4f + 0.9f * std::sin(t * 2.6f);
    list.arc(cx, cy, radius, radius * 0.18f, t * 5.2f, sweep, color);
}

Rect scaled(const Rect &r, float s)
{
    const float w = r.w * s, h = r.h * s;
    return {r.cx() - w * 0.5f, r.cy() - h * 0.5f, w, h};
}

void draw_card_skeleton(DrawList &list, const Context &ctx, const Rect &card, float title_w, float sub_w, float alpha)
{
    draw_skeleton(list, ctx, card, kRadius, alpha);
    draw_skeleton(list, ctx, {card.x, card.y + card.h + tw::kTitleGap - 20, card.w * title_w, 24}, 8, alpha);
    draw_skeleton(list, ctx, {card.x, card.y + card.h + tw::kTitleGap + 16, card.w * sub_w, 20}, 8, alpha);
}

/* ------------------------------------------------------------------- cards */

TwitchPicture twitch_picture(const evo_hui_twitch_image_t &in)
{
    TwitchPicture out;
    if (in.rgba && in.w > 0 && in.h > 0)
    {
        out.px = reinterpret_cast<const std::uint32_t *>(in.rgba);
        out.w = in.w;
        out.h = in.h;
        out.id = in.id ? in.id : 1;
    }
    return out;
}

std::uint32_t twitch_texture(const Context &ctx, const TwitchPicture &pic)
{
    if (!pic.px)
        return 0;
    return ctx.textures.pixels_immutable(pic.id, pic.px, pic.w, pic.h);
}

std::vector<std::string> fit_lines(const ui::FontRef &font, const std::string &text, float size, float width,
                                   int max_lines)
{
    std::vector<std::string> lines = font.font->wrap(text, size, width);
    const std::size_t most = static_cast<std::size_t>(std::max(1, max_lines));
    if (lines.size() > most)
    {
        std::string rest = lines[most - 1];
        for (std::size_t i = most; i < lines.size(); ++i)
            rest += " " + lines[i];
        lines.resize(most);
        lines[most - 1] = font.font->fit(rest, size, width);
    }
    /* A single word wider than the line comes back whole: cut it. */
    for (std::string &line : lines)
        if (font.measure(line, size) > width)
            line = font.font->fit(line, size, width);
    return lines;
}

std::string drawable(const ui::FontRef &font, const std::string &text)
{
    std::string out;
    out.reserve(text.size());
    bool gap = false; /* characters were left out since the last one kept */
    for (std::size_t i = 0; i < text.size();)
    {
        const std::size_t start = i;
        std::uint32_t cp = ::hui::gfx::next_codepoint(text, &i);
        if (cp < 0x20 || cp == 0x7f)
            cp = ' ';
        if (cp == ' ')
        {
            if (!out.empty() && out.back() != ' ')
                out.push_back(' ');
            gap = false;
            continue;
        }
        if (!font.font->has_glyph(cp))
        {
            gap = true;
            continue;
        }
        const bool punct = cp < 0x80 && std::strchr(".,!?:;)]}'\"", static_cast<int>(cp)) != nullptr;
        if (gap && punct && !out.empty() && out.back() == ' ')
            out.pop_back(); /* "Sonntag ☕:" reads "Sonntag:" */
        else if (gap && !punct && !out.empty() && out.back() != ' ')
            out.push_back(' ');
        gap = false;
        out.append(text, start, i - start);
    }
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

const std::string &FittedText::get(const ui::FontRef &font, const std::string &text, float size, float width) const
{
    if (font.font != font_ || size != size_ || width != width_ || text != text_)
    {
        font_ = font.font;
        size_ = size;
        width_ = width;
        text_ = text;
        out_ = fit(font, drawable(font, text), size, width);
    }
    return out_;
}

namespace
{

/* Copies src into dst when it differs; true when it did. */
bool take(std::string &dst, const char *src)
{
    src = src ? src : "";
    if (dst == src)
        return false;
    dst = src;
    return true;
}

} // namespace

bool assign(TwitchStream &s, const evo_hui_twitch_stream_t &in)
{
    const bool other = take(s.login, in.login);
    bool changed = other;
    changed |= take(s.name, in.name);
    changed |= take(s.title, in.title);
    changed |= take(s.game, in.game);
    changed |= s.viewers != in.viewers;
    s.viewers = in.viewers;
    if (changed)
        ++s.rev;
    s.thumb = twitch_picture(in.thumb);
    return other;
}

bool assign(TwitchCategory &c, const evo_hui_twitch_category_t &in)
{
    const bool other = take(c.name, in.name);
    if (other || c.viewers != in.viewers)
        ++c.rev;
    c.viewers = in.viewers;
    c.art = twitch_picture(in.art);
    return other;
}

bool assign(TwitchChannel &c, const evo_hui_twitch_channel_t &in)
{
    const bool other = take(c.login, in.login);
    bool changed = other;
    changed |= take(c.name, in.name);
    changed |= c.viewers != in.viewers;
    c.viewers = in.viewers;
    if (changed)
        ++c.rev;
    c.avatar = twitch_picture(in.avatar);
    return other;
}

namespace
{

const CardLines &stream_lines(const Context &ctx, const TwitchStream &s, float width)
{
    CardLines &l = s.lines;
    if (l.fonts == &ctx.fonts && l.width == width && l.rev == s.rev)
        return l;
    const ui::Fonts &fonts = ctx.fonts;
    l.fonts = &ctx.fonts;
    l.width = width;
    l.rev = s.rev;
    std::string title = drawable(fonts.semibold, s.title);
    std::string sub = drawable(fonts.regular, s.name);
    if (sub.empty())
        sub = s.login; /* a name all in a script the font lacks */
    const std::string game = drawable(fonts.regular, s.game);
    if (title.empty())
    {
        /* A title all in a script the fonts lack (Japanese, Korean ...) would
         * leave its line blank: the channel takes it, the category the line
         * under it. */
        title = drawable(fonts.semibold, s.name);
        if (title.empty())
            title = s.login;
        sub = game;
    }
    else if (!game.empty())
        sub += " \xC2\xB7 " + game; /* U+00B7 middle dot */
    l.title = fit(fonts.semibold, title, 26, width);
    l.sub = fit(fonts.regular, sub, 24, width);
    l.chip = format_viewers(s.viewers);
    l.chip_w = fonts.semibold.measure(l.chip, 18) + 20.0f;
    return l;
}

const CardLines &category_lines(const Context &ctx, const TwitchCategory &c, float width)
{
    CardLines &l = c.lines;
    if (l.fonts == &ctx.fonts && l.width == width && l.rev == c.rev)
        return l;
    const ui::Fonts &fonts = ctx.fonts;
    l.fonts = &ctx.fonts;
    l.width = width;
    l.rev = c.rev;
    const std::vector<std::string> name = fit_lines(fonts.semibold, drawable(fonts.semibold, c.name), 26, width, 2);
    l.title = name.empty() ? std::string() : name[0];
    l.title2 = name.size() > 1 ? name[1] : std::string();
    l.sub = c.viewers >= 0 ? fit(fonts.regular, format_viewers(c.viewers) + " watching", 24, width) : std::string();
    return l;
}

} // namespace

void CardMotion::track(const TwitchPicture &pic)
{
    if (!pic.px)
        art_in = 0.0f;
    art_id = pic.px ? pic.id : 0;
}

void CardMotion::step(float dt, bool group_focused, bool focused)
{
    focus.target = focused ? 1.0f : 0.0f;
    focus.update(dt, 16.0f);
    dim.target = group_focused && !focused ? 1.0f : 0.0f;
    dim.update(dt, 12.0f);
    if (art_id)
        art_in = std::min(1.0f, art_in + dt * 4.0f);
}

Color CardMotion::tint() const
{
    return ::hui::gfx::mix(kWhite, Color::rgb(0xa8a8b0), dim.value);
}

void draw_stream_card(DrawList &list, const Context &ctx, const TwitchStream &s, const CardMotion &m, const Rect &r,
                      bool focused, bool starting)
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    /* Only the picture grows: the lines under it stay on their neighbours'
     * baselines. */
    const float grow = tween::lerp(1.0f, tw::kFocusScale, m.focus.value);
    list.push_transform(grow, r.cx(), r.cy(), 0, 0);

    list.shadow({r.x, r.y + 12, r.w, r.h}, kRadius, 22, Color::rgb(0x000000, 0.25f + 0.3f * m.focus.value));
    const std::uint32_t tex = twitch_texture(ctx, s.thumb);
    if (m.art_in < 1.0f)
        draw_skeleton(list, ctx, r, kRadius);
    if (tex)
        list.image(tex, r, cover_uv(s.thumb.w, s.thumb.h, r.w, r.h), m.tint().with_alpha(m.art_in), kRadius);

    if (starting)
    {
        list.rounded_rect(r, kRadius, Color::rgb(0x000000, 0.62f));
        draw_spinner(list, ctx, r.cx(), r.cy() - 16, 22, kWhite);
        ui::text(list, fonts.semibold, "Starting\xE2\x80\xA6", r.cx(), r.cy() + 44, 24, kWhite, Align::center);
    }
    const CardLines &lines = stream_lines(ctx, s, r.w);
    if (!starting)
    {
        draw_live_pill(list, ctx, r.x + 10, r.y + 10, 15);
        const Rect chip{r.x + 10, r.y + r.h - 40, lines.chip_w, 30};
        list.rounded_rect(chip, 6, Color::rgb(0x000000, 0.72f));
        ui::text(list, fonts.semibold, lines.chip, chip.cx(), chip.y + 21, 18, kWhite, Align::center);
    }
    list.pop_transform();

    const float title_y = r.y + r.h + tw::kTitleGap;
    ui::text(list, fonts.semibold, lines.title, r.x, title_y, 26, focused ? p.text : p.text.with_alpha(0.9f));
    ui::text(list, fonts.regular, lines.sub, r.x, title_y + tw::kSubGap, 24, p.text_muted);
}

void draw_category_card(DrawList &list, const Context &ctx, const TwitchCategory &c, const CardMotion &m,
                        const Rect &r, bool focused)
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    const float grow = tween::lerp(1.0f, tw::kFocusScale, m.focus.value);
    list.push_transform(grow, r.cx(), r.cy(), 0, 0);
    list.shadow({r.x, r.y + 12, r.w, r.h}, kRadius, 22, Color::rgb(0x000000, 0.25f + 0.3f * m.focus.value));
    const std::uint32_t tex = twitch_texture(ctx, c.art);
    if (m.art_in < 1.0f)
        draw_skeleton(list, ctx, r, kRadius);
    if (tex)
        list.image(tex, r, cover_uv(c.art.w, c.art.h, r.w, r.h), m.tint().with_alpha(m.art_in), kRadius);
    list.pop_transform();

    /* The name takes two lines when it needs them ("Grand Theft" / "Auto V"
     * on Home's narrow cards); the viewers follow it. */
    const CardLines &lines = category_lines(ctx, c, r.w);
    const Color ink = focused ? p.text : p.text.with_alpha(0.9f);
    float y = r.y + r.h + tw::kTitleGap;
    ui::text(list, fonts.semibold, lines.title, r.x, y, 26, ink);
    if (!lines.title2.empty())
    {
        y += tw::kSubGap;
        ui::text(list, fonts.semibold, lines.title2, r.x, y, 26, ink);
    }
    if (!lines.sub.empty())
        ui::text(list, fonts.regular, lines.sub, r.x, y + tw::kSubGap, 24, p.text_muted);
}

/* -------------------------------------------------------------- focus ring */

Rect focus_ring_rect(const Rect &card)
{
    return scaled(card, tw::kFocusScale).inset(-7.0f);
}

void draw_focus_glow(DrawList &list, const Context &ctx, const Rect &ring, float alpha, float radius)
{
    if (alpha <= 0.01f)
        return;
    list.push_opacity(alpha);
    const float r = radius < 0.0f ? 18.0f : std::max(0.0f, radius - 6.0f);
    list.glow(ring.inset(6), r, 40, ctx.palette.accent.with_alpha(0.5f + 0.15f * ui::breathe(ctx.time)));
    list.pop_opacity();
}

void draw_focus_ring(DrawList &list, const Context &ctx, const Rect &ring, float alpha, float radius)
{
    if (alpha <= 0.01f)
        return;
    list.push_opacity(alpha);
    list.bordered_rect(ring, radius < 0.0f ? kFocusRingRadius : radius, kClear, 4.0f, ctx.palette.accent);
    list.pop_opacity();
}

/* ------------------------------------------------------ messages and hints */

void draw_twitch_message(DrawList &list, const Context &ctx, const Rect &area, const std::string &text,
                         const char *hint, float alpha, bool panel, ui::Button button)
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    list.push_opacity(alpha);
    if (panel)
    {
        list.rounded_rect(area, 16, p.surface.with_alpha(0.6f));
        list.bordered_rect(area, 16, kClear, 1.5f, kWhite.with_alpha(0.06f));
    }
    const float cx = area.cx();
    const std::string what = text.empty() ? "Something went wrong" : text;
    ui::text(list, fonts.display, what, cx, area.cy() - 6, 40, p.text, Align::center);
    draw_press_hint(list, ctx, cx, area.cy() + 50, button, hint);
    list.pop_opacity();
}

void draw_press_hint(DrawList &list, const Context &ctx, float cx, float base, ui::Button button, const char *hint)
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    /* "Press [button] <hint>", centred as one line. */
    const std::string after = str(hint);
    const float size = 24.0f, glyph = 34.0f;
    const float w1 = fonts.regular.measure("Press", size);
    const float wg = ui::button_width(button, glyph);
    const float w2 = fonts.regular.measure(after, size);
    const float total = w1 + 10 + wg + 10 + w2;
    float x = cx - total * 0.5f;
    ui::text(list, fonts.regular, "Press", x, base, size, p.text_muted);
    x += w1 + 10;
    ui::draw_button(list, fonts, ui::GlyphStyle::dark(), button, x, base - 8, glyph);
    x += wg + 10;
    ui::text(list, fonts.regular, after, x, base, size, p.text_muted);
}

void draw_message_mark(DrawList &list, const Context &ctx, float y, float alpha)
{
    list.push_opacity(alpha);
    const float cx = (kLeft + kRight) * 0.5f;
    /* The pages leave 106 px under y for the mark; the lettering, 144 x 72,
     * sits in the middle of that. */
    draw_app_lettering(list, ctx, cx - 72, y + 17, 144, ::hui::gfx::mix(ctx.palette.bg_top, kWhite, 0.16f));
    list.pop_opacity();
}

void draw_row_note(DrawList &list, const Context &ctx, const char *title, float y, const std::string &sentence,
                   const char *hint)
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    float x = kLeft + fonts.semibold.measure(title, 30) + 32;
    if (!hint)
    {
        ui::text(list, fonts.regular, sentence, x, y, 24, p.text_muted);
        return;
    }
    x += ui::text(list, fonts.regular, sentence + ". Press", x, y, 24, p.text_muted) + 10;
    ui::draw_button(list, fonts, ui::GlyphStyle::dark(), ui::Button::triangle, x, y - 8, 32);
    x += ui::button_width(ui::Button::triangle, 32) + 10;
    ui::text(list, fonts.regular, hint, x, y, 24, p.text_muted);
}

void draw_row_counter(DrawList &list, const Context &ctx, float y, int index, int count)
{
    char counter[32];
    std::snprintf(counter, sizeof counter, "%d of %d", index, count);
    ui::text(list, ctx.fonts.regular, counter, kRight, y, 22, ctx.palette.text_faint, Align::right);
}

void draw_twitch_hints(DrawList &list, const Context &ctx, const ui::Hint *hints, int count, float alpha)
{
    const Palette &p = ctx.palette;
    /* The page fades out under the hint bar. */
    list.gradient_rect({0, 984, 1920, 40}, 0, p.bg_bottom.with_alpha(0.0f), p.bg_bottom.with_alpha(0.75f));
    list.gradient_rect({0, 1024, 1920, 56}, 0, p.bg_bottom.with_alpha(0.75f), p.bg_bottom.with_alpha(1.0f));
    if (count <= 0)
        return;
    list.push_opacity(alpha);
    ui::HintLayout layout;
    layout.size = 34;
    layout.text_size = 24;
    layout.cy = 1036;
    ui::draw_hints(list, ctx.fonts, ui::GlyphStyle::dark(), hints, count, kRight, true, layout);
    list.pop_opacity();
}

int rail_hints(ui::Hint *out)
{
    /* ScreenManager::handleInput: Cross opens the rail's entry, Circle and
     * Right go back to the page, and nothing else gets through. */
    out[0] = {ui::Button::cross, "Select"};
    out[1] = {ui::Button::circle, "Back"};
    return 2;
}

void draw_watch_button(DrawList &list, const Context &ctx, const Rect &b, float f, bool starting)
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    const float round = b.h * 0.5f;
    if (f > 0.01f)
        list.glow(b, round, 26, p.accent.with_alpha(0.5f * f));
    list.rounded_rect(b, round, ::hui::gfx::mix(kWhite.with_alpha(0.12f), kWhite, f));
    const Color ink = ::hui::gfx::mix(kWhite, Color::rgb(0x0e0e10), f);
    if (starting)
    {
        draw_spinner(list, ctx, b.x + 38, b.cy(), 14, ink);
        ui::text(list, fonts.semibold, "STARTING", b.x + 68, b.cy() + 9, 24, ink, Align::left, 2.0f);
        return;
    }
    ui::draw_button(list, fonts, f > 0.5f ? ui::GlyphStyle::light() : ui::GlyphStyle::dark(), ui::Button::cross,
                    b.x + 20, b.cy(), 36);
    ui::text(list, fonts.semibold, "WATCH LIVE", b.x + 72, b.cy() + 9, 24, ink, Align::left, 2.0f);
}

/* ------------------------------------------------------------------- grids */

namespace
{
/* The grid keeps this much room over the row above the cursor: little
 * enough that the lines of the row before that are off the screen. A grid
 * whose lines all end above the hint band never scrolls. */
constexpr float kGridTopRoom = 24.0f;
constexpr float kGridBottom = kHintBandTop;
} // namespace

TwitchGrid TwitchGrid::make(int cols, float aspect, float top, float row_gap, float text_h)
{
    TwitchGrid g;
    g.cols = std::max(1, cols);
    g.top = top;
    g.row_gap = row_gap;
    g.text_h = text_h;
    g.card_w = std::floor((tw::kView - tw::kGap * static_cast<float>(g.cols - 1)) / static_cast<float>(g.cols));
    g.card_h = std::round(g.card_w * aspect);
    return g;
}

Rect TwitchGrid::cell(int index) const
{
    const int row = index / cols, col = index % cols;
    return {tw::kLeft + static_cast<float>(col) * (card_w + tw::kGap), top + static_cast<float>(row) * pitch(), card_w,
            card_h};
}

float TwitchGrid::page_for(int index, int count) const
{
    const int n = rows(count);
    const float end = top + static_cast<float>(n - 1) * pitch() + card_h + text_h;
    if (n <= 1 || index < cols || end <= kGridBottom)
        return 0.0f;
    /* No stop short of the last row's place: one would leave the lines of a
     * row cut off at the top of the screen. The last row sits where every
     * other row does, with room under it. */
    const float above = top + static_cast<float>(index / cols - 1) * pitch(); /* the row over the cursor */
    return std::max(0.0f, above - kGridTopRoom);
}

void draw_hint_band(DrawList &list, const Context &ctx)
{
    const Palette &p = ctx.palette;
    list.gradient_rect({0, kGridBottom, 1920, 40}, 0, p.bg_bottom.with_alpha(0.0f), p.bg_bottom.with_alpha(0.97f));
    list.gradient_rect({0, kGridBottom + 40, 1920, 1080 - kGridBottom - 40}, 0, p.bg_bottom.with_alpha(0.97f),
                       p.bg_bottom.with_alpha(1.0f));
}

void TwitchGridMotion::enter(float page_target, bool ready)
{
    age = 0.0f;
    ring_snapped = false;
    ring_alpha.snap(0.0f);
    page.snap(page_target);
    /* set() ran first: a grid that already has its cards shows them at once
     * and only takes part in the entrance. */
    in.snap(ready ? 1.0f : 0.0f);
}

void TwitchGridMotion::update(float dt, float page_target, const Rect &ring_target, bool ready)
{
    age += dt;
    page.target = page_target;
    page.update(dt, 10.0f);
    in.target = ready ? 1.0f : 0.0f;
    in.update(dt, 9.0f);
    const bool on_card = ring_target.w > 0.0f;
    if (!ring_snapped && on_card)
    {
        /* The first place the ring has: no flight in from the corner. */
        ring.snap(ring_target);
        page.snap(page.target);
        ring_snapped = true;
    }
    if (on_card)
        ring.target(ring_target);
    ring.update(dt, 20.0f);
    ring_alpha.target = on_card ? 1.0f : 0.0f;
    ring_alpha.update(dt, 14.0f);
}

} // namespace evo::kit
