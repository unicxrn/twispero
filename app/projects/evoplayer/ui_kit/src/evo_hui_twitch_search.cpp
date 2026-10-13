/*
 * evo_hui_twitch_search.cpp - Search on ps5-homebrew-ui.
 *
 * Top to bottom: "Search" with the field beside it (a magnifier and the
 * query, or what to type), then two rows like Home's: "Channels" (round
 * avatars, the name, live or not) and "Categories" (Home's 3:4 cards). Both
 * rows sit on the same columns, and all of it fits above the hint band, so
 * the page never scrolls and every line under a card can be read wherever the
 * cursor is. One ring glides between the cards; on an avatar it is a circle.
 * The field lights up instead of taking the ring, like Home's Watch button.
 * Before a query is typed, the recent searches (stage 7) line up under the
 * field as pills of its width, newest first; each lights up as the field does
 * when it has the cursor.
 *
 * core/src/screens/TwitchSearchScreen.cpp owns the query, the data and the
 * cursor and fills evo_hui_twitch_search_params_t; this only draws it.
 */
#include "evo_hui_twitch.hpp"

#include "ui/glyphs.hpp"

#include <algorithm>
#include <cmath>

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
constexpr float kView = tw::kView;
constexpr float kGap = tw::kGap;

/* Both rows on Home's category columns: a channel's avatar is centred over
 * the column a category card fills. */
constexpr float kColW = 216.0f;
constexpr float kAvatar = 96.0f;
constexpr float kCatH = 288.0f; /* 3:4 */

/* The header: Browse's title, and the field beside it from the second
 * column to the right edge, centred on the title's capitals. */
constexpr float kTitleY = 136.0f;
const Rect kField{kLeft + kColW + kGap, 80, kRight - (kLeft + kColW + kGap), 72};
constexpr float kFieldRadius = 36.0f;
constexpr float kFieldText = 30.0f;

/* The rows start where Browse's grid does, give or take the row title. */
constexpr float kRow1Title = 240.0f;
constexpr float kRow1Top = kRow1Title + 24.0f; /* the avatars' top */
constexpr float kRow2Title = kRow1Top + kAvatar + tw::kTextH + 52.0f;
constexpr float kRow2Top = kRow2Title + 24.0f;
static_assert(kRow2Top + kCatH + tw::kCatTextH <= kHintBandTop,
              "a category's last line must end above the hint band");

/* A message instead of the rows: Browse's spot for its own. With no query
 * yet, only its hint, closer under the mark. */
constexpr float kMarkY = 352.0f;
const Rect kMessage{(kLeft + kRight) * 0.5f - 600.0f, 470, 1200, 160};
constexpr float kNoneHintY = 520.0f;

constexpr const char *kPlaceholder = "Search for a channel or a game";

/* The recent searches: pills the field's width, under a small title. */
constexpr float kRecentTitleY = 236.0f;
constexpr float kRecentTop = 268.0f;
constexpr float kRecentH = 64.0f;
constexpr float kRecentGap = 12.0f;
constexpr float kRecentText = 28.0f;
static_assert(kRecentTop + EVO_HUI_TWITCH_SEARCH_RECENTS * (kRecentH + kRecentGap) - kRecentGap <= kHintBandTop,
              "every recent search fits above the hint band");

std::string str(const char *s)
{
    return std::string(s ? s : "");
}

/* The magnifier, centred on (cx, cy), `size` across. */
void draw_magnifier(DrawList &list, float cx, float cy, float size, Color ink)
{
    const float stroke = size * 0.1f;
    const float rad = size * 0.32f;
    const float lx = cx - size * 0.08f, ly = cy - size * 0.08f; /* the lens sits up and left */
    list.ring(lx, ly, rad, stroke, ink);
    const float d = rad * 0.7071f;
    list.line(lx + d, ly + d, cx + size * 0.42f, cy + size * 0.42f, stroke, ink);
}

/* A clock face, centred on (cx, cy), `size` across: a search made before. */
void draw_clock(DrawList &list, float cx, float cy, float size, Color ink)
{
    const float stroke = size * 0.1f;
    const float rad = size * 0.42f;
    list.ring(cx, cy, rad, stroke, ink);
    list.line(cx, cy, cx, cy - rad * 0.58f, stroke, ink);
    list.line(cx, cy, cx + rad * 0.45f, cy + rad * 0.3f, stroke, ink);
}

/* A channel's lines, worked out again when its text, the fonts or the width
 * change: the name, and "N watching" or "Offline". */
const CardLines &channel_lines(const Context &ctx, const TwitchChannel &c, float width)
{
    CardLines &l = c.lines;
    if (l.fonts == &ctx.fonts && l.width == width && l.rev == c.rev)
        return l;
    const ui::Fonts &fonts = ctx.fonts;
    l.fonts = &ctx.fonts;
    l.width = width;
    l.rev = c.rev;
    std::string name = drawable(fonts.semibold, c.name);
    l.title = fonts.semibold.font->fit(name.empty() ? c.login : name, 26, width);
    if (c.viewers >= 0)
    {
        /* The red dot goes in front: room for it. */
        l.sub = fonts.regular.font->fit(format_viewers(c.viewers) + " watching", 24, width - 18);
        l.chip_w = fonts.regular.measure(l.sub, 24) + 18;
    }
    else
    {
        l.sub = "Offline";
        l.chip_w = fonts.regular.measure(l.sub, 24);
    }
    return l;
}

/* A channel: the avatar in a circle `r` (it grows with m.focus), the name
 * and whether it is live centred under it. */
void draw_channel_tile(DrawList &list, const Context &ctx, const TwitchChannel &c, const CardMotion &m, const Rect &r,
                       bool focused)
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    const float round = r.w * 0.5f;
    const float grow = tween::lerp(1.0f, tw::kFocusScale, m.focus.value);
    list.push_transform(grow, r.cx(), r.cy(), 0, 0);
    list.shadow({r.x, r.y + 10, r.w, r.h}, round, 18, Color::rgb(0x000000, 0.25f + 0.3f * m.focus.value));
    const std::uint32_t tex = twitch_texture(ctx, c.avatar);
    if (m.art_in < 1.0f)
        draw_skeleton(list, ctx, r, round);
    if (tex)
        list.image(tex, r, cover_uv(c.avatar.w, c.avatar.h, r.w, r.h), m.tint().with_alpha(m.art_in), round);
    list.bordered_rect(r, round, kClear, 1.5f, kWhite.with_alpha(0.08f));
    list.pop_transform();

    const CardLines &lines = channel_lines(ctx, c, kColW);
    const float cx = r.cx();
    const float name_y = r.y + r.h + tw::kTitleGap;
    ui::text(list, fonts.semibold, lines.title, cx, name_y, 26, focused ? p.text : p.text.with_alpha(0.9f),
             Align::center);
    const float sub_y = name_y + tw::kSubGap;
    const float x = cx - lines.chip_w * 0.5f;
    if (c.viewers >= 0)
    {
        list.circle(x + 5, sub_y - 8, 5, twitch_live_red());
        ui::text(list, fonts.regular, lines.sub, x + 18, sub_y, 24, p.text_muted);
    }
    else
    {
        ui::text(list, fonts.regular, lines.sub, x, sub_y, 24, p.text_muted.with_alpha(0.78f));
    }
}

} // namespace

/* -------------------------------------------------------------------- state */

void TwitchSearchScreen::set(const evo_hui_twitch_search_params_t &p)
{
    query_ = str(p.query);
    channels_status_ = p.channels_status;
    channels_error_ = str(p.channels_error);
    const int chn = std::clamp(p.channel_count, 0, EVO_HUI_TWITCH_SEARCH_CHANNELS);
    channels_.resize(static_cast<std::size_t>(chn));
    for (int i = 0; i < chn; ++i)
    {
        TwitchChannel &c = channels_[static_cast<std::size_t>(i)];
        if (assign(c, p.channels[i]))
            channel_motion_[i].art_in = 0.0f; /* another channel in this slot */
        channel_motion_[i].track(c.avatar);
    }

    categories_status_ = p.categories_status;
    categories_error_ = str(p.categories_error);
    const int cn = std::clamp(p.category_count, 0, EVO_HUI_TWITCH_SEARCH_CATEGORIES);
    categories_.resize(static_cast<std::size_t>(cn));
    for (int i = 0; i < cn; ++i)
    {
        TwitchCategory &c = categories_[static_cast<std::size_t>(i)];
        if (assign(c, p.categories[i]))
            category_motion_[i].art_in = 0.0f;
        category_motion_[i].track(c.art);
    }

    const int rn = std::clamp(p.recent_count, 0, EVO_HUI_TWITCH_SEARCH_RECENTS);
    recents_.resize(static_cast<std::size_t>(rn));
    for (int i = 0; i < rn; ++i)
        recents_[static_cast<std::size_t>(i)] = str(p.recents[i]);
    recent_cursor_ = std::clamp(p.recent_cursor, 0, std::max(0, rn - 1));

    focus_row_ = p.focus_row;
    if (focus_row_ == EVO_HUI_TWITCH_SEARCH_RECENT && (rn == 0 || !query_.empty()))
        focus_row_ = EVO_HUI_TWITCH_SEARCH_FIELD;
    rail_focused_ = p.rail_focused != 0;
    channel_col_ = std::clamp(p.channel_col, 0, std::max(0, chn - 1));
    category_col_ = std::clamp(p.category_col, 0, std::max(0, cn - 1));
}

void TwitchSearchScreen::enter()
{
    age_ = 0.0f;
    ring_snapped_ = false;
    ring_alpha_.snap(0.0f);
    field_focus_.snap(!rail_focused_ && focus_row_ == EVO_HUI_TWITCH_SEARCH_FIELD ? 1.0f : 0.0f);
    for (int i = 0; i < EVO_HUI_TWITCH_SEARCH_RECENTS; ++i)
        recent_focus_[i].snap(!rail_focused_ && focus_row_ == EVO_HUI_TWITCH_SEARCH_RECENT && i == recent_cursor_
                                  ? 1.0f
                                  : 0.0f);
    /* set() ran first: rows that already have their results show them at
     * once and only take part in the entrance. */
    channels_in_.snap(channels_status_ == EVO_HUI_TWITCH_READY && !channels_.empty() ? 1.0f : 0.0f);
    categories_in_.snap(categories_status_ == EVO_HUI_TWITCH_READY && !categories_.empty() ? 1.0f : 0.0f);
}

TwitchSearchScreen::Body TwitchSearchScreen::body() const
{
    if (query_.empty())
        return recents_.empty() ? Body::none : Body::recent;
    const bool ch_done = channels_status_ != EVO_HUI_TWITCH_LOADING;
    const bool cat_done = categories_status_ != EVO_HUI_TWITCH_LOADING;
    if (!(ch_done && cat_done && channels_.empty() && categories_.empty()))
        return Body::rows;
    if (channels_status_ == EVO_HUI_TWITCH_ERROR || categories_status_ == EVO_HUI_TWITCH_ERROR)
        return Body::failed;
    return Body::nothing;
}

Rect TwitchSearchScreen::avatar_rect(int col) const
{
    const float x = kLeft + static_cast<float>(col) * (kColW + kGap) - channel_scroll_.offset();
    return {x + (kColW - kAvatar) * 0.5f, kRow1Top, kAvatar, kAvatar};
}

Rect TwitchSearchScreen::category_rect(int col) const
{
    return {kLeft + static_cast<float>(col) * (kColW + kGap) - category_scroll_.offset(), kRow2Top, kColW, kCatH};
}

Rect TwitchSearchScreen::recent_rect(int index) const
{
    return {kField.x, kRecentTop + static_cast<float>(index) * (kRecentH + kRecentGap), kField.w, kRecentH};
}

Rect TwitchSearchScreen::focus_target() const
{
    /* Where the cursor lands once the rows stop (see Home). */
    if (rail_focused_)
        return {};
    if (focus_row_ == EVO_HUI_TWITCH_SEARCH_CHANNEL && !channels_.empty())
    {
        Rect r = avatar_rect(channel_col_);
        r.x += channel_scroll_.offset() - channel_scroll_.position.target;
        return focus_ring_rect(r);
    }
    if (focus_row_ == EVO_HUI_TWITCH_SEARCH_CATEGORY && !categories_.empty())
    {
        Rect r = category_rect(category_col_);
        r.x += category_scroll_.offset() - category_scroll_.position.target;
        return focus_ring_rect(r);
    }
    /* The field: the ring waits there, unseen, over the channel the cursor
     * would land on, so a move down drops it straight onto the avatar. */
    Rect r = avatar_rect(channel_col_);
    r.x += channel_scroll_.offset() - channel_scroll_.position.target;
    return {r.x, kField.y, r.w, kField.h};
}

void TwitchSearchScreen::update(float dt)
{
    age_ += dt;

    const int chn = static_cast<int>(channels_.size());
    if (chn > 0)
    {
        const float start = static_cast<float>(channel_col_) * (kColW + kGap);
        const float limit = static_cast<float>(chn) * (kColW + kGap) - kGap;
        channel_scroll_.reveal(start, start + kColW, kView, kColW * 0.8f, limit);
    }
    const int cn = static_cast<int>(categories_.size());
    if (cn > 0)
    {
        const float start = static_cast<float>(category_col_) * (kColW + kGap);
        const float limit = static_cast<float>(cn) * (kColW + kGap) - kGap;
        category_scroll_.reveal(start, start + kColW, kView, kColW * 0.8f, limit);
    }
    channel_scroll_.update(dt, 12.0f);
    category_scroll_.update(dt, 12.0f);

    channels_in_.target = channels_status_ == EVO_HUI_TWITCH_READY && chn > 0 ? 1.0f : 0.0f;
    channels_in_.update(dt, 9.0f);
    categories_in_.target = categories_status_ == EVO_HUI_TWITCH_READY && cn > 0 ? 1.0f : 0.0f;
    categories_in_.update(dt, 9.0f);

    const bool on_field = !rail_focused_ && focus_row_ == EVO_HUI_TWITCH_SEARCH_FIELD;
    const bool on_recent = !rail_focused_ && focus_row_ == EVO_HUI_TWITCH_SEARCH_RECENT;
    const Rect target = focus_target();
    const bool on_card = target.w > 0.0f && !on_field && !on_recent;
    const float round = focus_row_ == EVO_HUI_TWITCH_SEARCH_CHANNEL ? 1.0f : 0.0f;
    if (!ring_snapped_ && target.w > 0.0f)
    {
        ring_.snap(target);
        ring_round_.snap(round);
        channel_scroll_.position.snap(channel_scroll_.position.target);
        category_scroll_.position.snap(category_scroll_.position.target);
        ring_snapped_ = true;
    }
    if (target.w > 0.0f)
        ring_.target(target);
    ring_.update(dt, 20.0f);
    ring_round_.target = round;
    ring_round_.update(dt, 20.0f);
    ring_alpha_.target = on_card ? 1.0f : 0.0f;
    ring_alpha_.update(dt, 14.0f);
    field_focus_.target = on_field ? 1.0f : 0.0f;
    field_focus_.update(dt, 16.0f);
    for (int i = 0; i < EVO_HUI_TWITCH_SEARCH_RECENTS; ++i)
    {
        recent_focus_[i].target = on_recent && i == recent_cursor_ ? 1.0f : 0.0f;
        recent_focus_[i].update(dt, 16.0f);
    }

    const bool on_channels = !rail_focused_ && focus_row_ == EVO_HUI_TWITCH_SEARCH_CHANNEL;
    const bool on_categories = !rail_focused_ && focus_row_ == EVO_HUI_TWITCH_SEARCH_CATEGORY;
    for (int i = 0; i < EVO_HUI_TWITCH_SEARCH_CHANNELS; ++i)
        channel_motion_[i].step(dt, on_channels, on_channels && i == channel_col_);
    for (int i = 0; i < EVO_HUI_TWITCH_SEARCH_CATEGORIES; ++i)
        category_motion_[i].step(dt, on_categories, on_categories && i == category_col_);
}

/* -------------------------------------------------------------------- field */

void TwitchSearchScreen::draw_field(DrawList &list, const Context &ctx, float alpha) const
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    const float f = field_focus_.value;
    const Rect r = kField;
    list.push_opacity(alpha);
    if (f > 0.01f)
        list.glow(r, kFieldRadius, 26, p.accent.with_alpha(0.35f * f));
    list.rounded_rect(r, kFieldRadius, ::hui::gfx::mix(p.surface, p.surface_sel, f));
    list.rounded_rect(r, kFieldRadius, kWhite.with_alpha(0.03f + 0.03f * f));
    if (f > 0.01f)
        list.bordered_rect(r, kFieldRadius, kClear, 3.0f, p.accent.with_alpha(f));
    list.bordered_rect(r, kFieldRadius, kClear, 1.5f, kWhite.with_alpha(0.08f * (1.0f - f)));

    draw_magnifier(list, r.x + 44, r.cy(), 30, ::hui::gfx::mix(p.text_muted, p.text, f));
    const float tx = r.x + 84;
    const float base = r.cy() + kFieldText * 0.36f;
    if (query_.empty())
        ui::text(list, fonts.regular, kPlaceholder, tx, base, kFieldText, p.text_faint);
    else
        ui::text(list, fonts.regular, query_fit_.get(fonts.regular, query_, kFieldText, r.x + r.w - 40 - tx), tx, base,
                 kFieldText, p.text);
    list.pop_opacity();
}

/* ------------------------------------------------------------ recent searches */

void TwitchSearchScreen::draw_recents(DrawList &list, const Context &ctx, float alpha) const
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    const int n = static_cast<int>(recents_.size());
    list.push_opacity(alpha);
    ui::text(list, fonts.semibold, "Recent searches", kField.x + 28, kRecentTitleY, 26, p.text_muted);
    for (int i = 0; i < n; ++i)
    {
        /* One after another, as the rows of the other pages come in. */
        const float in = tween::stagger(age_, 2 + i, 0.04f, 0.45f);
        const float f = recent_focus_[i].value;
        const Rect r = recent_rect(i);
        const float radius = r.h * 0.5f;
        list.push_opacity(in);
        list.push_transform(1.0f, 0, 0, 0, 16.0f * (1.0f - in));
        if (f > 0.01f)
            list.glow(r, radius, 22, p.accent.with_alpha(0.3f * f));
        list.rounded_rect(r, radius, ::hui::gfx::mix(p.surface, p.surface_sel, f).with_alpha(0.75f + 0.25f * f));
        if (f > 0.01f)
            list.bordered_rect(r, radius, kClear, 3.0f, p.accent.with_alpha(f));
        list.bordered_rect(r, radius, kClear, 1.5f, kWhite.with_alpha(0.06f * (1.0f - f)));
        draw_clock(list, r.x + 44, r.cy(), 26, ::hui::gfx::mix(p.text_faint, p.text_muted, f));
        const float tx = r.x + 84;
        const std::string &words =
            recent_fit_[i].get(fonts.regular, recents_[static_cast<std::size_t>(i)], kRecentText, r.x + r.w - 40 - tx);
        ui::text(list, fonts.regular, words, tx, r.cy() + kRecentText * 0.36f, kRecentText,
                 ::hui::gfx::mix(p.text.with_alpha(0.82f), p.text, f));
        list.pop_transform();
        list.pop_opacity();
    }
    list.pop_opacity();
}

/* --------------------------------------------------------------------- rows */

void TwitchSearchScreen::draw_channels(DrawList &list, const Context &ctx, float alpha) const
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    list.push_opacity(alpha);
    ui::text(list, fonts.semibold, "Channels", kLeft, kRow1Title, 30, p.text);
    const int n = static_cast<int>(channels_.size());
    if (!rail_focused_ && focus_row_ == EVO_HUI_TWITCH_SEARCH_CHANNEL && n > 0)
        draw_row_counter(list, ctx, kRow1Title, channel_col_ + 1, n);

    if (n == 0 && channels_status_ != EVO_HUI_TWITCH_LOADING)
    {
        if (channels_status_ == EVO_HUI_TWITCH_ERROR)
            draw_row_note(list, ctx, "Channels", kRow1Title,
                          channels_error_.empty() ? std::string("Something went wrong") : channels_error_,
                          "to try again");
        else
            draw_row_note(list, ctx, "Channels", kRow1Title, "No matching channels", nullptr);
        list.pop_opacity();
        return;
    }
    const float in = channels_in_.value;
    if (in < 0.99f)
    {
        for (int i = 0; i < 7; ++i)
        {
            const float cx = kLeft + static_cast<float>(i) * (kColW + kGap) + kColW * 0.5f;
            const Rect r{cx - kAvatar * 0.5f, kRow1Top, kAvatar, kAvatar};
            const float a = 1.0f - in;
            draw_skeleton(list, ctx, r, kAvatar * 0.5f, a);
            draw_skeleton(list, ctx, {cx - 70, r.y + r.h + tw::kTitleGap - 20, 140, 24}, 8, a);
            draw_skeleton(list, ctx, {cx - 50, r.y + r.h + tw::kTitleGap + 16, 100, 20}, 8, a);
        }
    }
    if (in > 0.01f)
    {
        list.push_opacity(in);
        int focused = -1;
        for (int col = 0; col < n; ++col)
        {
            const Rect r = avatar_rect(col);
            if (r.x - 60.0f > 1920.0f || r.x + r.w + 60.0f < NavRail::kWidth - 40.0f)
                continue;
            if (!rail_focused_ && focus_row_ == EVO_HUI_TWITCH_SEARCH_CHANNEL && col == channel_col_)
            {
                focused = col;
                continue;
            }
            draw_channel_tile(list, ctx, channels_[static_cast<std::size_t>(col)], channel_motion_[col], r, false);
        }
        if (focused >= 0)
            draw_channel_tile(list, ctx, channels_[static_cast<std::size_t>(focused)], channel_motion_[focused],
                              avatar_rect(focused), true);
        list.pop_opacity();
    }
    list.pop_opacity();
}

void TwitchSearchScreen::draw_categories(DrawList &list, const Context &ctx, float alpha) const
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    list.push_opacity(alpha);
    ui::text(list, fonts.semibold, "Categories", kLeft, kRow2Title, 30, p.text);
    const int n = static_cast<int>(categories_.size());
    if (!rail_focused_ && focus_row_ == EVO_HUI_TWITCH_SEARCH_CATEGORY && n > 0)
        draw_row_counter(list, ctx, kRow2Title, category_col_ + 1, n);

    if (n == 0 && categories_status_ != EVO_HUI_TWITCH_LOADING)
    {
        if (categories_status_ == EVO_HUI_TWITCH_ERROR)
            draw_row_note(list, ctx, "Categories", kRow2Title,
                          categories_error_.empty() ? std::string("Something went wrong") : categories_error_,
                          "to try again");
        else
            draw_row_note(list, ctx, "Categories", kRow2Title, "No matching categories", nullptr);
        list.pop_opacity();
        return;
    }
    const float in = categories_in_.value;
    if (in < 0.99f)
    {
        for (int i = 0; i < 8; ++i)
        {
            const Rect r{kLeft + static_cast<float>(i) * (kColW + kGap), kRow2Top, kColW, kCatH};
            draw_card_skeleton(list, ctx, r, 0.8f, 0.5f, 1.0f - in);
        }
    }
    if (in > 0.01f)
    {
        list.push_opacity(in);
        int focused = -1;
        for (int col = 0; col < n; ++col)
        {
            const Rect r = category_rect(col);
            if (r.x > 1920.0f || r.x + r.w < NavRail::kWidth - 40.0f)
                continue;
            if (!rail_focused_ && focus_row_ == EVO_HUI_TWITCH_SEARCH_CATEGORY && col == category_col_)
            {
                focused = col;
                continue;
            }
            draw_category_card(list, ctx, categories_[static_cast<std::size_t>(col)], category_motion_[col], r, false);
        }
        if (focused >= 0)
            draw_category_card(list, ctx, categories_[static_cast<std::size_t>(focused)], category_motion_[focused],
                               category_rect(focused), true);
        list.pop_opacity();
    }
    list.pop_opacity();
}

/* --------------------------------------------------------------------- draw */

void TwitchSearchScreen::draw(DrawList &list, const Context &ctx) const
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    const Body what = body();
    draw_backdrop(list, ctx);

    list.push_clip({NavRail::kWidth, 0, ::hui::gfx::kVirtualWidth - NavRail::kWidth, ::hui::gfx::kVirtualHeight});
    {
        const float a = tween::stagger(age_, 0, 0.05f, 0.45f);
        list.push_transform(1.0f, 0, 0, 0, 16.0f * (1.0f - a));
        list.push_opacity(a);
        ui::text(list, fonts.display, "Search", kLeft - 2, kTitleY, 56, p.text);
        list.pop_opacity();
        draw_field(list, ctx, a);
        list.pop_transform();
    }

    const float in = tween::stagger(age_, 1, 0.08f, 0.6f);
    if (what == Body::recent)
    {
        draw_recents(list, ctx, in);
    }
    else if (what == Body::none)
    {
        /* Nothing asked yet: the field says what to type; how, under the mark. */
        draw_message_mark(list, ctx, kMarkY, in);
        list.push_opacity(in);
        draw_press_hint(list, ctx, kMessage.cx(), kNoneHintY, ui::Button::cross, "to start typing");
        list.pop_opacity();
    }
    else if (what == Body::failed)
    {
        draw_message_mark(list, ctx, kMarkY, in);
        const std::string &why = channels_status_ == EVO_HUI_TWITCH_ERROR ? channels_error_ : categories_error_;
        draw_twitch_message(list, ctx, kMessage, why, "to try again", in, false);
    }
    else if (what == Body::nothing)
    {
        draw_message_mark(list, ctx, kMarkY, in);
        /* No results for “<query>”, the query cut to fit. */
        const std::string before = "No results for \xE2\x80\x9C", after = "\xE2\x80\x9D";
        const float room = kMessage.w - fonts.display.measure(before + after, 40);
        const std::string what_text = before + none_fit_.get(fonts.display, query_, 40, std::max(room, 120.0f)) + after;
        draw_twitch_message(list, ctx, kMessage, what_text, "to try another search", in, false, ui::Button::cross);
    }
    else if (what == Body::rows)
    {
        /* The ring's light under the cards, the ring over them. */
        const Rect ring = ring_.value();
        const float radius = tween::lerp(kFocusRingRadius, ring.w * 0.5f, ring_round_.value);
        draw_focus_glow(list, ctx, ring, ring_alpha_.value, radius);
        {
            const float a = tween::stagger(age_, 2, 0.09f, 0.6f);
            list.push_transform(1.0f, 0, 0, 0, 40.0f * (1.0f - a));
            draw_channels(list, ctx, a);
            list.pop_transform();
        }
        {
            const float a = tween::stagger(age_, 3, 0.09f, 0.6f);
            list.push_transform(1.0f, 0, 0, 0, 40.0f * (1.0f - a));
            draw_categories(list, ctx, a);
            list.pop_transform();
        }
        draw_focus_ring(list, ctx, ring, ring_alpha_.value, radius);
    }
    list.pop_clip();
    if (what == Body::rows)
        draw_hint_band(list, ctx);

    ui::Hint hints[4];
    int h = 0;
    if (rail_focused_)
        h = rail_hints(hints);
    else
    {
        if (focus_row_ == EVO_HUI_TWITCH_SEARCH_FIELD)
        {
            hints[h++] = {ui::Button::cross, "Search"};
            if (!query_.empty())
                hints[h++] = {ui::Button::square, "Clear"};
        }
        else if (focus_row_ == EVO_HUI_TWITCH_SEARCH_RECENT && !recents_.empty())
        {
            hints[h++] = {ui::Button::cross, "Search"};
            hints[h++] = {ui::Button::square, "Remove"};
        }
        else if (focus_row_ == EVO_HUI_TWITCH_SEARCH_CHANNEL && !channels_.empty())
            hints[h++] = {ui::Button::cross, "Channel"}; /* its page, as Square says elsewhere */
        else if (focus_row_ == EVO_HUI_TWITCH_SEARCH_CATEGORY && !categories_.empty())
            hints[h++] = {ui::Button::cross, "Open"};
        if (!query_.empty())
            hints[h++] = {ui::Button::triangle, "Refresh"};
        hints[h++] = {ui::Button::circle, "Back"};
    }
    draw_twitch_hints(list, ctx, hints, h, tween::stagger(age_, 6, 0.08f, 0.5f));
}

/* -------------------------------------------------------------------- focus */

bool TwitchSearchScreen::focus(FocusInfo *out) const
{
    out->focusable.clear();
    out->focusable.emplace_back("field", query_.empty() ? std::string(kPlaceholder) : query_);
    for (std::size_t i = 0; i < channels_.size(); ++i)
        out->focusable.emplace_back("channel-" + std::to_string(i), channels_[i].name);
    for (std::size_t i = 0; i < categories_.size(); ++i)
        out->focusable.emplace_back("category-" + std::to_string(i), categories_[i].name);
    if (query_.empty())
        for (std::size_t i = 0; i < recents_.size(); ++i)
            out->focusable.emplace_back("recent-" + std::to_string(i), recents_[i]);
    if (rail_focused_)
    {
        out->focusable.clear();
        return false;
    }
    if (focus_row_ == EVO_HUI_TWITCH_SEARCH_CHANNEL && !channels_.empty())
    {
        const TwitchChannel &c = channels_[static_cast<std::size_t>(channel_col_)];
        out->id = "channel-" + std::to_string(channel_col_);
        out->text = c.viewers >= 0 ? c.name + ", live" : c.name + ", offline";
        out->index = channel_col_ + 1;
        out->total = static_cast<int>(channels_.size());
        out->rect = avatar_rect(channel_col_);
        return true;
    }
    if (focus_row_ == EVO_HUI_TWITCH_SEARCH_RECENT && !recents_.empty())
    {
        out->id = "recent-" + std::to_string(recent_cursor_);
        out->text = recents_[static_cast<std::size_t>(recent_cursor_)];
        out->index = recent_cursor_ + 1;
        out->total = static_cast<int>(recents_.size());
        out->rect = recent_rect(recent_cursor_);
        return true;
    }
    if (focus_row_ == EVO_HUI_TWITCH_SEARCH_CATEGORY && !categories_.empty())
    {
        out->id = "category-" + std::to_string(category_col_);
        out->text = categories_[static_cast<std::size_t>(category_col_)].name;
        out->index = category_col_ + 1;
        out->total = static_cast<int>(categories_.size());
        out->rect = category_rect(category_col_);
        return true;
    }
    out->id = "field";
    out->text = query_.empty() ? std::string(kPlaceholder) : query_;
    out->index = out->total = 1;
    out->rect = kField;
    return true;
}

} // namespace evo::kit
