/*
 * evo_hui_twitch_videos.cpp - what a channel page and a category share for
 * their videos and clips (stage 7): the video card, the tab row and the
 * filter chips. See evo_hui_twitch.hpp.
 */
#include "evo_hui_twitch.hpp"

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
const Color kClear = Color::rgb(0x000000, 0.0f);
const Color kInk = Color::rgb(0x0e0e10); /* dark text on a white chip, as on Watch live */
constexpr float kRadius = tw::kRadius;

/* Copies src into dst when it differs; true when it did. */
bool take(std::string &dst, const char *src)
{
    src = src ? src : "";
    if (dst == src)
        return false;
    dst = src;
    return true;
}

/* The lines under a video card, and its length chip. */
const CardLines &video_lines(const Context &ctx, const TwitchVideo &v, float width)
{
    CardLines &l = v.lines;
    if (l.fonts == &ctx.fonts && l.width == width && l.rev == v.rev)
        return l;
    const ui::Fonts &fonts = ctx.fonts;
    l.fonts = &ctx.fonts;
    l.width = width;
    l.rev = v.rev;
    l.title = fonts.semibold.font->fit(drawable(fonts.semibold, v.title), 26, width);
    std::string sub;
    if (v.views >= 0)
        sub = format_viewers(v.views) + (v.views == 1 ? " view" : " views");
    if (!v.age.empty())
        sub += (sub.empty() ? "" : " \xC2\xB7 ") + v.age; /* U+00B7 middle dot */
    l.sub = fonts.regular.font->fit(sub, 24, width);
    l.title2 = fonts.regular.font->fit(drawable(fonts.regular, v.detail), 24, width);
    l.chip = !v.chip.empty() ? v.chip : v.seconds > 0 ? format_duration(v.seconds) : std::string();
    l.chip_w = l.chip.empty() ? 0.0f : fonts.semibold.measure(l.chip, 18) + 20.0f;
    return l;
}

} // namespace

/* ------------------------------------------------------------------- pieces */

std::string format_duration(int seconds)
{
    char b[24];
    const int s = std::max(0, seconds);
    if (s >= 3600)
        std::snprintf(b, sizeof b, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    else
        std::snprintf(b, sizeof b, "%d:%02d", s / 60, s % 60);
    return b;
}

const char *const kVideoFilterLabels[EVO_HUI_TWITCH_VIDEO_FILTERS] = {"Past broadcasts", "Highlights", "Uploads"};
/* Every period in the same words ("Last 7 days", not "7 days" beside
 * "Last 24 hours"). */
const char *const kClipFilterLabels[EVO_HUI_TWITCH_CLIP_FILTERS] = {"Last 24 hours", "Last 7 days",
                                                                     "Last 30 days", "All time"};

const char *empty_videos_text(int filter)
{
    switch (filter)
    {
    case EVO_HUI_TWITCH_VIDEOS_HIGHLIGHT:
        return "No highlights yet";
    case EVO_HUI_TWITCH_VIDEOS_UPLOAD:
        return "No uploads yet";
    default:
        return "No past broadcasts right now";
    }
}

const char *empty_clips_text(int filter)
{
    switch (filter)
    {
    case EVO_HUI_TWITCH_CLIPS_DAY:
        return "No clips from the last 24 hours";
    case EVO_HUI_TWITCH_CLIPS_MONTH:
        return "No clips from the last 30 days";
    case EVO_HUI_TWITCH_CLIPS_ALL:
        return "No clips yet";
    default:
        return "No clips from the last 7 days";
    }
}

/* -------------------------------------------------------------------- cards */

bool assign(TwitchVideo &v, const evo_hui_twitch_video_t &in)
{
    const bool other = take(v.id, in.id);
    bool changed = other;
    changed |= take(v.title, in.title);
    changed |= take(v.detail, in.detail);
    changed |= take(v.age, in.age);
    changed |= take(v.chip, in.chip);
    changed |= v.views != in.views || v.seconds != in.seconds;
    v.views = in.views;
    v.seconds = in.seconds;
    if (changed)
        ++v.rev;
    v.progress = in.progress;
    v.thumb = twitch_picture(in.thumb);
    return other;
}

void draw_video_card(DrawList &list, const Context &ctx, const TwitchVideo &v, const CardMotion &m, const Rect &r,
                     bool focused, bool starting)
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    /* As a stream card: only the picture grows, the lines stay on their
     * neighbours' baselines. */
    const float grow = tween::lerp(1.0f, tw::kFocusScale, m.focus.value);
    list.push_transform(grow, r.cx(), r.cy(), 0, 0);

    list.shadow({r.x, r.y + 12, r.w, r.h}, kRadius, 22, Color::rgb(0x000000, 0.25f + 0.3f * m.focus.value));
    const std::uint32_t tex = twitch_texture(ctx, v.thumb);
    if (m.art_in < 1.0f)
        draw_skeleton(list, ctx, r, kRadius);
    if (tex)
        list.image(tex, r, cover_uv(v.thumb.w, v.thumb.h, r.w, r.h), m.tint().with_alpha(m.art_in), kRadius);

    const CardLines &lines = video_lines(ctx, v, r.w);
    if (starting)
    {
        list.rounded_rect(r, kRadius, Color::rgb(0x000000, 0.62f));
        draw_spinner(list, ctx, r.cx(), r.cy() - 16, 22, kWhite);
        ui::text(list, fonts.semibold, "Starting\xE2\x80\xA6", r.cx(), r.cy() + 44, 24, kWhite, Align::center);
    }
    else
    {
        /* The length where a live card has LIVE, in a stream card's chip. */
        if (!lines.chip.empty())
        {
            const Rect chip{r.x + 10, r.y + 10, lines.chip_w, 30};
            list.rounded_rect(chip, 6, Color::rgb(0x000000, 0.72f));
            ui::text(list, fonts.semibold, lines.chip, chip.cx(), chip.y + 21, 18, kWhite, Align::center);
        }
        /* How much was watched, along the foot. */
        if (v.progress >= 0.0f)
        {
            const Rect track{r.x + 12, r.y + r.h - 16, r.w - 24, 6};
            list.rounded_rect(track, 3, Color::rgb(0x000000, 0.55f));
            list.rounded_rect(track, 3, kWhite.with_alpha(0.22f));
            const float done = std::max(track.h, track.w * std::min(1.0f, v.progress));
            list.rounded_rect({track.x, track.y, done, track.h}, 3, p.accent);
        }
    }
    list.pop_transform();

    const float title_y = r.y + r.h + tw::kTitleGap;
    ui::text(list, fonts.semibold, lines.title, r.x, title_y, 26, focused ? p.text : p.text.with_alpha(0.9f));
    ui::text(list, fonts.regular, lines.sub, r.x, title_y + tw::kSubGap, 24, p.text_muted);
    /* A step quieter than the line above, still easy to read on a TV
     * (text_faint is too dim for 24 px from the couch). */
    if (!lines.title2.empty())
        ui::text(list, fonts.regular, lines.title2, r.x, title_y + 2.0f * tw::kSubGap, 24,
                 p.text_muted.with_alpha(0.78f));
}

/* ----------------------------------------------------------- tabs and chips */

namespace
{
constexpr float kTabSize = 28.0f;
constexpr float kTabSpacing = 56.0f; /* one label's end to the next one's start */
constexpr float kTabPad = 22.0f;     /* the focus ring's room either side of a label */
} // namespace

Rect tab_rect(int index, float y)
{
    /* As if every label were "Videos" wide: close enough for a report. */
    constexpr float kRough = 94.0f;
    return {tw::kLeft - kTabPad + static_cast<float>(index) * (kRough + kTabSpacing), y, kRough + 2.0f * kTabPad,
            tw::kTabH};
}

void draw_tab_row(DrawList &list, const Context &ctx, const char *const *labels, int count, int open, float line,
                  float y, float alpha, float focus)
{
    if (alpha <= 0.01f || count <= 0)
        return;
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    float xs[4], ws[4];
    count = std::min(count, 4);
    float x = tw::kLeft;
    for (int i = 0; i < count; ++i)
    {
        xs[i] = x;
        ws[i] = fonts.semibold.measure(labels[i], kTabSize);
        x += ws[i] + kTabSpacing;
    }
    list.push_opacity(alpha);
    const float base = y + tw::kTabH * 0.5f + 10.0f;
    for (int i = 0; i < count; ++i)
        ui::text(list, fonts.semibold, labels[i], xs[i], base, kTabSize, i == open ? p.text : p.text_muted);

    /* The open tab's line, under its label: it slides from one tab to the
     * other, as wide as the label it is under. */
    const float at = std::clamp(line, 0.0f, static_cast<float>(count - 1));
    const int a = static_cast<int>(std::floor(at));
    const int b = std::min(count - 1, a + 1);
    const float f = at - static_cast<float>(a);
    const float lx = tween::lerp(xs[a], xs[b], f), lw = tween::lerp(ws[a], ws[b], f);
    list.rounded_rect({lx, y + tw::kTabH - 8, lw, 4}, 2, p.accent);

    /* The cursor: the ring, hugging the open tab's label. */
    if (focus > 0.01f && open >= 0 && open < count)
    {
        const Rect r{xs[open] - kTabPad, y, ws[open] + 2.0f * kTabPad, tw::kTabH};
        list.push_opacity(focus);
        list.rounded_rect(r, r.h * 0.5f, kWhite.with_alpha(0.06f));
        list.bordered_rect(r.inset(-7.0f), r.h * 0.5f + 7.0f, kClear, 4.0f, p.accent);
        list.pop_opacity();
    }
    list.pop_opacity();
}

Rect chip_rect(int index, float y, float chip_w)
{
    return {tw::kLeft + static_cast<float>(index) * (chip_w + tw::kChipGap), y, chip_w, tw::kChipH};
}

void draw_chip_row(DrawList &list, const Context &ctx, const char *const *labels, int count, int selected, float y,
                   float chip_w, float alpha)
{
    if (alpha <= 0.01f)
        return;
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    list.push_opacity(alpha);
    for (int i = 0; i < count; ++i)
    {
        const Rect r = chip_rect(i, y, chip_w);
        const float round = r.h * 0.5f;
        if (i == selected)
        {
            list.rounded_rect(r, round, kWhite.with_alpha(0.94f));
            ui::text(list, fonts.semibold, labels[i], r.cx(), r.cy() + 8, 22, kInk, Align::center);
        }
        else
        {
            list.rounded_rect(r, round, kWhite.with_alpha(0.07f));
            list.bordered_rect(r, round, kClear, 1.5f, kWhite.with_alpha(0.08f));
            ui::text(list, fonts.semibold, labels[i], r.cx(), r.cy() + 8, 22, p.text.with_alpha(0.82f),
                     Align::center);
        }
    }
    list.pop_opacity();
}

} // namespace evo::kit
