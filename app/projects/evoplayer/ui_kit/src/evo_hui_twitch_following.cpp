/*
 * evo_hui_twitch_following.cpp - Following on ps5-homebrew-ui.
 *
 * Under Browse's header ("Following" and the counts): "Live now", the
 * followed channels that are live as 16:9 stream cards four to a row, like a
 * category's grid, then "Offline", the others as round avatars with their
 * names, seven to a row. One ring glides through both grids (a circle on an
 * avatar, as on Search), and the page scrolls on a spring so the cursor's row
 * sits second from the top with the row above it in full, as on a category;
 * the header scrolls away with it. While a list loads its section shows
 * skeletons; a section with nothing in it says so on its title's line.
 * Logged out, the page is one message: how to log in.
 *
 * core/src/screens/TwitchFollowingScreen.cpp owns the data, the login state
 * and the cursor and fills evo_hui_twitch_following_params_t; this only
 * draws it.
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

constexpr float kLeft = tw::kLeft;
constexpr float kRight = tw::kRight;
constexpr float kGap = tw::kGap;

/* The header: Browse's title and the line under it. */
constexpr float kTitleY = 136.0f;
constexpr float kLineY = 184.0f;

/* The sections: a title (semibold 30, on its baseline) over each grid. */
constexpr float kLiveTitle = 268.0f;
constexpr float kSectionTop = 24.0f;  /* a title's baseline to the stream cards' top */
constexpr float kSectionGap = 64.0f;  /* the last line of the live cards to the Offline title */
constexpr float kNoteGap = 84.0f;     /* a title with only a note on its line to the next title */

constexpr int kLiveCols = EVO_HUI_TWITCH_FOLLOWING_LIVE_COLS;
constexpr int kAvatarCols = EVO_HUI_TWITCH_FOLLOWING_CHANNEL_COLS;
/* The avatars: a circle centred in each of seven columns, the name under it,
 * and for a live one "N watching". Rows far enough apart that a focused
 * avatar's ring stays clear of the lines above it. */
constexpr float kColW = (tw::kView - kGap * static_cast<float>(kAvatarCols - 1)) / static_cast<float>(kAvatarCols);
constexpr float kAvatar = 128.0f;
constexpr float kAvatarTop = 32.0f;     /* the title's baseline to the circles' top: room for the ring */
constexpr float kAvatarPitch = kAvatar + 100.0f;
constexpr float kAvatarTextH = 84.0f;   /* the lines under a circle, descenders included */

/* The page keeps this much room over the row above the cursor (TwitchGrid's). */
constexpr float kTopRoom = 24.0f;

/* Where a message goes when there is nothing to show: Browse's spot. */
constexpr float kMarkY = 352.0f;
const Rect kMessage{(kLeft + kRight) * 0.5f - 600.0f, 470, 1200, 160};

std::string str(const char *s)
{
    return std::string(s ? s : "");
}

/* The header goes as the page leaves it. */
float header_alpha(float page)
{
    return 1.0f - tween::clamp01(page / 160.0f);
}

/* Rows come in one after another, rising a little (Browse's). */
float row_in(float age, int row)
{
    return tween::stagger(age, 1 + std::min(row, 4), 0.06f, 0.5f);
}

/* Something on the page that reaches `bottom` is worth drawing when some of
 * it is on screen. */
bool on_screen(float top, float bottom, float page)
{
    return top - page < ::hui::gfx::kVirtualHeight && bottom - page > 0.0f;
}

/* An avatar's lines, worked out again when its text, the fonts or the width
 * change: the name, and "N watching" when it is live. */
const CardLines &avatar_lines(const Context &ctx, const TwitchChannel &c, float width)
{
    CardLines &l = c.lines;
    if (l.fonts == &ctx.fonts && l.width == width && l.rev == c.rev)
        return l;
    const ui::Fonts &fonts = ctx.fonts;
    l.fonts = &ctx.fonts;
    l.width = width;
    l.rev = c.rev;
    const std::string name = drawable(fonts.semibold, c.name);
    l.title = fonts.semibold.font->fit(name.empty() ? c.login : name, 26, width);
    if (c.viewers >= 0)
    {
        /* The red dot goes in front: room for it. */
        l.sub = fonts.regular.font->fit(format_viewers(c.viewers) + " watching", 24, width - 18);
        l.chip_w = fonts.regular.measure(l.sub, 24) + 18;
    }
    else
    {
        l.sub.clear();
        l.chip_w = 0.0f;
    }
    return l;
}

/* A channel: the avatar in the circle `r` (it grows with m.focus), its name
 * centred under it and, when live, the viewers. */
void draw_avatar_tile(DrawList &list, const Context &ctx, const TwitchChannel &c, const CardMotion &m, const Rect &r,
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

    const CardLines &lines = avatar_lines(ctx, c, kColW - 8.0f);
    const float cx = r.cx();
    const float name_y = r.y + r.h + tw::kTitleGap;
    ui::text(list, fonts.semibold, lines.title, cx, name_y, 26, focused ? p.text : p.text.with_alpha(0.9f),
             Align::center);
    if (c.viewers >= 0)
    {
        const float sub_y = name_y + tw::kSubGap;
        const float x = cx - lines.chip_w * 0.5f;
        list.circle(x + 5, sub_y - 8, 5, twitch_live_red());
        ui::text(list, fonts.regular, lines.sub, x + 18, sub_y, 24, p.text_muted);
    }
}

} // namespace

/* -------------------------------------------------------------------- state */

void TwitchFollowingScreen::set(const evo_hui_twitch_following_params_t &p)
{
    login_state_ = p.login_state;
    live_status_ = p.live_status;
    live_error_ = str(p.live_error);
    const int ln = std::clamp(p.live_count, 0, EVO_HUI_TWITCH_FOLLOWING_LIVE);
    live_.resize(static_cast<std::size_t>(ln));
    for (int i = 0; i < ln; ++i)
    {
        TwitchStream &s = live_[static_cast<std::size_t>(i)];
        if (assign(s, p.live[i]))
            live_motion_[i].art_in = 0.0f; /* another channel in this slot */
        live_motion_[i].track(s.thumb);
    }

    channels_status_ = p.channels_status;
    channels_error_ = str(p.channels_error);
    const int cn = std::clamp(p.channel_count, 0, EVO_HUI_TWITCH_FOLLOWING_CHANNELS);
    channels_.resize(static_cast<std::size_t>(cn));
    for (int i = 0; i < cn; ++i)
    {
        TwitchChannel &c = channels_[static_cast<std::size_t>(i)];
        if (assign(c, p.channels[i]))
            channel_motion_[i].art_in = 0.0f;
        channel_motion_[i].track(c.avatar);
    }

    cursor_ = std::clamp(p.cursor, 0, std::max(0, card_count() - 1));
    rail_focused_ = p.rail_focused != 0;
    scope_missing_ = p.scope_missing != 0;
    starting_ = str(p.starting_login);

    /* "3 live · 42 channels", once both lists are in. */
    const bool known = login_state_ == EVO_HUI_TWITCH_FOLLOWING_LOGGED_IN && live_status_ == EVO_HUI_TWITCH_READY &&
                       channels_status_ == EVO_HUI_TWITCH_READY;
    /* The live list failed: the channels are still known ("42 channels"). */
    const bool channels_only = login_state_ == EVO_HUI_TWITCH_FOLLOWING_LOGGED_IN &&
                               live_status_ == EVO_HUI_TWITCH_ERROR && channels_status_ == EVO_HUI_TWITCH_READY;
    if (channels_only)
    {
        if (counts_for_[0] != -2 || counts_for_[1] != cn) /* -2: the channels alone */
        {
            counts_for_[0] = -2;
            counts_for_[1] = cn;
            char b[64];
            std::snprintf(b, sizeof b, "%d channel%s", cn, cn == 1 ? "" : "s");
            counts_ = b;
        }
    }
    else if (!known)
    {
        counts_.clear();
        counts_for_[0] = counts_for_[1] = -1;
    }
    else if (counts_for_[0] != ln || counts_for_[1] != cn)
    {
        counts_for_[0] = ln;
        counts_for_[1] = cn;
        char b[64];
        std::snprintf(b, sizeof b, "%d live \xC2\xB7 %d channel%s", ln, ln + cn, ln + cn == 1 ? "" : "s");
        counts_ = b;
    }
}

bool TwitchFollowingScreen::live_loading() const
{
    return login_state_ == EVO_HUI_TWITCH_FOLLOWING_CHECKING || live_status_ == EVO_HUI_TWITCH_LOADING;
}

bool TwitchFollowingScreen::channels_loading() const
{
    return login_state_ == EVO_HUI_TWITCH_FOLLOWING_CHECKING || channels_status_ == EVO_HUI_TWITCH_LOADING;
}

TwitchFollowingScreen::Body TwitchFollowingScreen::body() const
{
    if (login_state_ == EVO_HUI_TWITCH_FOLLOWING_LOGGED_OUT)
        return Body::logged_out;
    if (card_count() > 0 || live_loading() || channels_loading())
        return Body::grids;
    if (live_status_ == EVO_HUI_TWITCH_ERROR || channels_status_ == EVO_HUI_TWITCH_ERROR)
        return Body::failed;
    return Body::none;
}

TwitchFollowingScreen::Layout TwitchFollowingScreen::layout() const
{
    Layout l;
    l.live_title = kLiveTitle;
    l.live = TwitchGrid::make(kLiveCols, 9.0f / 16.0f, kLiveTitle + kSectionTop);
    const int ln = static_cast<int>(live_.size());
    l.live_rows = ln > 0 ? l.live.rows(ln) : (live_loading() ? 1 : 0);
    const float live_end = l.live_rows > 0 ? l.live.top + static_cast<float>(l.live_rows - 1) * l.live.pitch() +
                                                 l.live.card_h + l.live.text_h
                                           : kLiveTitle;
    l.offline_title = live_end + (l.live_rows > 0 ? kSectionGap : kNoteGap);
    l.avatars_top = l.offline_title + kAvatarTop;
    const int cn = static_cast<int>(channels_.size());
    l.avatar_rows = cn > 0 ? (cn + kAvatarCols - 1) / kAvatarCols : (channels_loading() ? 1 : 0);
    l.end = l.avatar_rows > 0
                ? l.avatars_top + static_cast<float>(l.avatar_rows - 1) * kAvatarPitch + kAvatar + kAvatarTextH
                : l.offline_title;
    return l;
}

Rect TwitchFollowingScreen::avatar_rect(const Layout &l, int index) const
{
    const int row = index / kAvatarCols, col = index % kAvatarCols;
    return {kLeft + static_cast<float>(col) * (kColW + kGap) + (kColW - kAvatar) * 0.5f,
            l.avatars_top + static_cast<float>(row) * kAvatarPitch, kAvatar, kAvatar};
}

int TwitchFollowingScreen::cursor_row() const
{
    const int ln = static_cast<int>(live_.size());
    if (cursor_ < ln)
        return cursor_ / kLiveCols;
    const int live_rows = (ln + kLiveCols - 1) / kLiveCols;
    return live_rows + (cursor_ - ln) / kAvatarCols;
}

float TwitchFollowingScreen::row_top(const Layout &l, int row) const
{
    const int live_rows = (static_cast<int>(live_.size()) + kLiveCols - 1) / kLiveCols;
    if (row < live_rows)
        return l.live.top + static_cast<float>(row) * l.live.pitch();
    return l.avatars_top + static_cast<float>(row - live_rows) * kAvatarPitch;
}

float TwitchFollowingScreen::page_target(const Layout &l) const
{
    /* TwitchGrid::page_for over both grids: nothing moves for the first row
     * or a page that fits; otherwise the row above the cursor's sits at the
     * top, in full (with the Offline title, when that is between them). */
    if (body() != Body::grids || card_count() == 0 || l.end <= kHintBandTop)
        return 0.0f;
    const int row = cursor_row();
    if (row == 0)
        return 0.0f;
    return std::max(0.0f, row_top(l, row - 1) - kTopRoom);
}

Rect TwitchFollowingScreen::ring_target(const Layout &l) const
{
    if (rail_focused_ || card_count() == 0 || body() != Body::grids)
        return {};
    const float page = page_target(l);
    Rect r = on_live() ? l.live.cell(cursor_) : avatar_rect(l, cursor_ - static_cast<int>(live_.size()));
    r.y -= page;
    return focus_ring_rect(r);
}

void TwitchFollowingScreen::enter()
{
    age_ = 0.0f;
    ring_snapped_ = false;
    ring_alpha_.snap(0.0f);
    page_.snap(page_target(layout()));
    /* set() ran first: a grid that already has its cards shows them at once
     * and only takes part in the entrance. */
    live_in_.snap(!live_loading() && !live_.empty() ? 1.0f : 0.0f);
    channels_in_.snap(!channels_loading() && !channels_.empty() ? 1.0f : 0.0f);
}

void TwitchFollowingScreen::update(float dt)
{
    age_ += dt;
    const Layout l = layout();
    page_.target = page_target(l);
    page_.update(dt, 10.0f);
    live_in_.target = !live_loading() && !live_.empty() ? 1.0f : 0.0f;
    live_in_.update(dt, 9.0f);
    channels_in_.target = !channels_loading() && !channels_.empty() ? 1.0f : 0.0f;
    channels_in_.update(dt, 9.0f);

    const Rect target = ring_target(l);
    const bool on_card = target.w > 0.0f;
    const float round = on_live() ? 0.0f : 1.0f;
    if (!ring_snapped_ && on_card)
    {
        /* The first place the ring has: no flight in from the corner. */
        ring_.snap(target);
        ring_round_.snap(round);
        page_.snap(page_.target);
        ring_snapped_ = true;
    }
    if (on_card)
        ring_.target(target);
    ring_.update(dt, 20.0f);
    ring_round_.target = round;
    ring_round_.update(dt, 20.0f);
    ring_alpha_.target = on_card ? 1.0f : 0.0f;
    ring_alpha_.update(dt, 14.0f);

    const bool focused = on_card;
    const int ln = static_cast<int>(live_.size());
    const bool in_live = focused && on_live(), in_channels = focused && !on_live();
    for (int i = 0; i < EVO_HUI_TWITCH_FOLLOWING_LIVE; ++i)
        live_motion_[i].step(dt, in_live, in_live && i == cursor_);
    for (int i = 0; i < EVO_HUI_TWITCH_FOLLOWING_CHANNELS; ++i)
        channel_motion_[i].step(dt, in_channels, in_channels && i == cursor_ - ln);
}

/* --------------------------------------------------------------------- draw */

void TwitchFollowingScreen::draw_header(DrawList &list, const Context &ctx, float page) const
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    const float in = tween::stagger(age_, 0, 0.05f, 0.45f);
    list.push_opacity(in * header_alpha(page));
    const float dy = 16.0f * (1.0f - in);
    ui::text(list, fonts.display, "Following", kLeft - 2, kTitleY + dy, 56, p.text);
    ui::text(list, fonts.regular, counts_.empty() ? std::string("Channels you follow") : counts_, kLeft, kLineY + dy,
             26, p.text_muted);
    list.pop_opacity();
}

void TwitchFollowingScreen::draw_live(DrawList &list, const Context &ctx, const Layout &l, float page) const
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    const float title_in = row_in(age_, 0);
    list.push_opacity(title_in);
    list.push_transform(1.0f, 0, 0, 0, 40.0f * (1.0f - title_in));
    ui::text(list, fonts.semibold, "Live now", kLeft, l.live_title, 30, p.text);
    const int n = static_cast<int>(live_.size());
    if (n == 0 && !live_loading())
    {
        if (live_status_ == EVO_HUI_TWITCH_ERROR)
            draw_row_note(list, ctx, "Live now", l.live_title,
                          live_error_.empty() ? std::string("Something went wrong") : live_error_,
                          scope_missing_ ? nullptr : "to try again");
        else
            draw_row_note(list, ctx, "Live now", l.live_title, "Nobody you follow is live right now", nullptr);
    }
    list.pop_transform();
    list.pop_opacity();
    if (n == 0 && !live_loading())
        return;

    const TwitchGrid &g = l.live;
    const float in = live_in_.value;
    if (in < 0.99f)
    {
        /* One row of skeletons: how many are live isn't known yet. */
        for (int i = 0; i < kLiveCols; ++i)
            draw_card_skeleton(list, ctx, g.cell(i), 0.82f, 0.56f, (1.0f - in) * row_in(age_, 0));
    }
    if (in > 0.01f)
    {
        list.push_opacity(in);
        const int focused = rail_focused_ || !on_live() ? -1 : cursor_;
        auto draw_one = [&](int i) {
            const Rect r = g.cell(i);
            const float a = row_in(age_, i / kLiveCols);
            list.push_opacity(a);
            list.push_transform(1.0f, 0, 0, 0, 40.0f * (1.0f - a));
            const TwitchStream &s = live_[static_cast<std::size_t>(i)];
            draw_stream_card(list, ctx, s, live_motion_[i], r, i == focused, starting_ == s.login);
            list.pop_transform();
            list.pop_opacity();
        };
        for (int i = 0; i < n; ++i)
        {
            const Rect r = g.cell(i);
            if (i != focused && on_screen(r.y, r.y + r.h + g.text_h, page))
                draw_one(i);
        }
        if (focused >= 0)
            draw_one(focused);
        list.pop_opacity();
    }
}

void TwitchFollowingScreen::draw_offline(DrawList &list, const Context &ctx, const Layout &l, float page) const
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    const int live_rows = (static_cast<int>(live_.size()) + kLiveCols - 1) / kLiveCols;
    const float title_in = row_in(age_, std::max(1, live_rows));
    list.push_opacity(title_in);
    list.push_transform(1.0f, 0, 0, 0, 40.0f * (1.0f - title_in));
    ui::text(list, fonts.semibold, "Offline", kLeft, l.offline_title, 30, p.text);
    const int n = static_cast<int>(channels_.size());
    if (n == 0 && !channels_loading())
    {
        if (channels_status_ == EVO_HUI_TWITCH_ERROR)
            draw_row_note(list, ctx, "Offline", l.offline_title,
                          channels_error_.empty() ? std::string("Something went wrong") : channels_error_,
                          scope_missing_ ? nullptr : "to try again");
        else
            draw_row_note(list, ctx, "Offline", l.offline_title, "Everyone you follow is live right now", nullptr);
    }
    list.pop_transform();
    list.pop_opacity();
    if (n == 0 && !channels_loading())
        return;

    const float in = channels_in_.value;
    if (in < 0.99f)
    {
        const float a = (1.0f - in) * title_in;
        for (int i = 0; i < kAvatarCols; ++i)
        {
            const Rect r = avatar_rect(l, i);
            const float cx = r.cx();
            draw_skeleton(list, ctx, r, kAvatar * 0.5f, a);
            draw_skeleton(list, ctx, {cx - 64, r.y + r.h + tw::kTitleGap - 20, 128, 24}, 8, a);
        }
    }
    if (in > 0.01f)
    {
        list.push_opacity(in);
        const int ln = static_cast<int>(live_.size());
        const int focused = rail_focused_ || on_live() ? -1 : cursor_ - ln;
        auto draw_one = [&](int i) {
            const Rect r = avatar_rect(l, i);
            const float a = row_in(age_, live_rows + i / kAvatarCols);
            list.push_opacity(a);
            list.push_transform(1.0f, 0, 0, 0, 40.0f * (1.0f - a));
            draw_avatar_tile(list, ctx, channels_[static_cast<std::size_t>(i)], channel_motion_[i], r, i == focused);
            list.pop_transform();
            list.pop_opacity();
        };
        for (int i = 0; i < n; ++i)
        {
            const Rect r = avatar_rect(l, i);
            if (i != focused && on_screen(r.y - 20.0f, r.y + r.h + kAvatarTextH, page))
                draw_one(i);
        }
        if (focused >= 0 && focused < n)
            draw_one(focused);
        list.pop_opacity();
    }
}

void TwitchFollowingScreen::draw(DrawList &list, const Context &ctx) const
{
    const Layout l = layout();
    const Body what = body();
    const float page = page_.value;
    draw_backdrop(list, ctx);

    list.push_clip({NavRail::kWidth, 0, ::hui::gfx::kVirtualWidth - NavRail::kWidth, ::hui::gfx::kVirtualHeight});
    list.push_transform(1.0f, 0, 0, 0, -page);
    draw_header(list, ctx, page);

    const float in = tween::stagger(age_, 1, 0.08f, 0.6f);
    if (what == Body::logged_out)
    {
        draw_message_mark(list, ctx, kMarkY, in);
        draw_twitch_message(list, ctx, kMessage, "Log in to see the channels you follow", "to log in", in, false,
                            ui::Button::cross);
    }
    else if (what == Body::failed)
    {
        draw_message_mark(list, ctx, kMarkY, in);
        const std::string &why = live_status_ == EVO_HUI_TWITCH_ERROR ? live_error_ : channels_error_;
        if (scope_missing_)
            draw_twitch_message(list, ctx, kMessage, why, "to open Settings", in, false, ui::Button::cross);
        else
            draw_twitch_message(list, ctx, kMessage, why, "to try again", in, false);
    }
    else if (what == Body::none)
    {
        draw_message_mark(list, ctx, kMarkY, in);
        draw_twitch_message(list, ctx, kMessage, "You don't follow any channels yet", "to refresh", in, false);
    }
    else
    {
        /* The ring's light under the cards, the ring over them; both in
         * screen space. */
        const Rect ring = ring_.value();
        const float radius = tween::lerp(kFocusRingRadius, ring.w * 0.5f, ring_round_.value);
        list.push_transform(1.0f, 0, 0, 0, page);
        draw_focus_glow(list, ctx, ring, ring_alpha_.value, radius);
        list.pop_transform();
        draw_live(list, ctx, l, page);
        draw_offline(list, ctx, l, page);
        list.push_transform(1.0f, 0, 0, 0, page);
        draw_focus_ring(list, ctx, ring, ring_alpha_.value, radius);
        list.pop_transform();
    }
    list.pop_transform();
    list.pop_clip();
    if (what == Body::grids)
        draw_hint_band(list, ctx);

    ui::Hint hints[4];
    int h = 0;
    if (rail_focused_)
        h = rail_hints(hints);
    else if (what == Body::logged_out)
    {
        hints[h++] = {ui::Button::cross, "Log in"};
        hints[h++] = {ui::Button::circle, "Back"};
    }
    else
    {
        if (what == Body::grids && card_count() > 0)
        {
            const bool live =
                on_live() || channels_[static_cast<std::size_t>(cursor_) - live_.size()].viewers >= 0;
            if (live)
            {
                hints[h++] = {ui::Button::cross, "Watch"};
                hints[h++] = {ui::Button::square, "Channel"};
            }
            else
                hints[h++] = {ui::Button::cross, "Channel"}; /* its page, as Square says elsewhere */
        }
        if (what == Body::failed && scope_missing_)
            hints[h++] = {ui::Button::cross, "Settings"};
        if (login_state_ == EVO_HUI_TWITCH_FOLLOWING_LOGGED_IN && !scope_missing_)
            hints[h++] = {ui::Button::triangle, "Refresh"};
        hints[h++] = {ui::Button::circle, "Back"};
    }
    draw_twitch_hints(list, ctx, hints, h, tween::stagger(age_, 6, 0.08f, 0.5f));
}

/* -------------------------------------------------------------------- focus */

bool TwitchFollowingScreen::focus(FocusInfo *out) const
{
    out->focusable.clear();
    if (rail_focused_)
        return false;
    const Body what = body();
    if (what == Body::logged_out)
    {
        out->id = "log-in";
        out->text = "Log in to see the channels you follow";
        out->index = out->total = 1;
        out->rect = kMessage;
        out->focusable.emplace_back(out->id, out->text);
        return true;
    }
    for (std::size_t i = 0; i < live_.size(); ++i)
        out->focusable.emplace_back("live-" + std::to_string(i), live_[i].name);
    for (std::size_t i = 0; i < channels_.size(); ++i)
        out->focusable.emplace_back("channel-" + std::to_string(i), channels_[i].name);
    if (what != Body::grids || card_count() == 0)
    {
        out->focusable.clear();
        return false;
    }
    const Layout l = layout();
    const float page = page_target(l);
    Rect r;
    if (on_live())
    {
        const TwitchStream &s = live_[static_cast<std::size_t>(cursor_)];
        out->id = "live-" + std::to_string(cursor_);
        out->text = s.name + ": " + s.title;
        r = l.live.cell(cursor_);
    }
    else
    {
        const int i = cursor_ - static_cast<int>(live_.size());
        const TwitchChannel &c = channels_[static_cast<std::size_t>(i)];
        out->id = "channel-" + std::to_string(i);
        out->text = c.viewers >= 0 ? c.name + ", live" : c.name + ", offline";
        r = avatar_rect(l, i);
    }
    out->index = cursor_ + 1;
    out->total = card_count();
    r.y -= page;
    out->rect = r;
    return true;
}

} // namespace evo::kit
