/*
 * evo_hui_twitch_home.cpp - the Twitch home screen on ps5-homebrew-ui.
 *
 * Top to bottom: the most-watched stream as the hero (picture on the right,
 * who and what on the left), "Popular now" (16:9 stream cards) and "Top
 * categories" (3:4 box art). While logged in with followed channels live,
 * "Following · live now" (stream cards too) takes Popular now's place and the
 * two rows under it slide down one row. "Recently watched" (round avatars on
 * the category columns, a live channel ringed in red) comes next when there
 * are channels in the watch history, then "Continue watching" (video cards
 * the size of stream cards, with the watched part along their foot) when
 * there are videos to pick up again; the rows under each slide down to make
 * room. The page scrolls up when the cursor goes down past the first row,
 * bringing the cursor's row and the one above it to the middle; each row
 * scrolls sideways on a spring. One focus ring glides between cards (a
 * circle on an avatar), and the focused card grows a little. While a row
 * loads it shows skeletons in the shape of its cards.
 *
 * core/src/screens/TwitchHomeScreen.cpp owns the data and the cursor and
 * fills evo_hui_twitch_home_params_t; this only draws it.
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

/* Layout, in page coordinates (the page scrolls up by page_). */
constexpr float kLeft = tw::kLeft;
constexpr float kRight = tw::kRight;
constexpr float kView = tw::kView;

/* The hero is short enough that both rows' titles show over the hint band,
 * with the top of the category art under the second one. */
const Rect kHeroArt{1024, 64, 800, 450};
constexpr float kHeroTextW = 680.0f;
/* The text block sits on the picture's bottom edge: Watch ends where the
 * picture ends, and the lines stack up from there. */
const Rect kHeroButton{kLeft, kHeroArt.y + kHeroArt.h - tw::kWatchH, tw::kWatchW, tw::kWatchH};
constexpr float kHeroTitleSize = 30.0f;
constexpr float kHeroTitleLead = 42.0f;

constexpr float kGap = tw::kGap;
constexpr float kStreamW = EVO_HUI_TWITCH_HOME_STREAM_W;
constexpr float kStreamH = 171.0f; /* 16:9 */
constexpr float kRow1Title = 590.0f;
constexpr float kRow1Top = 614.0f;
constexpr float kCatW = EVO_HUI_TWITCH_HOME_CATEGORY_W;
constexpr float kCatH = 288.0f; /* 3:4 */
constexpr float kRow2Title = 922.0f;
constexpr float kRow2Top = 946.0f;
constexpr float kTitleCap = 22.0f; /* a row title's cap height over its baseline */
/* With the followed row there, Popular now and the categories sit one row
 * further down, and so on for each row that comes in above them. */
constexpr float kRowPitch = kRow2Title - kRow1Title;
constexpr float kCardsTop = kRow1Top - kRow1Title; /* a row's title baseline to its cards */
/* Recently watched: round avatars centred in Home's category columns, the
 * name (and the viewers of a live one) under each, as on Following. Its row
 * is shorter than a stream row by what its circles are. */
constexpr float kRecentW = EVO_HUI_TWITCH_HOME_RECENT_W;
constexpr float kRecentAvatar = 120.0f;
constexpr float kRecentAvatarTop = 32.0f; /* the title's baseline to the circles' top: room for the ring */
constexpr float kRecentPitch = kRecentAvatarTop + kRecentAvatar + tw::kTextH +
                               (kRowPitch - (kCardsTop + kStreamH + tw::kTextH));

/* core/ works out which cards are drawn from the same numbers. */
static_assert(kLeft == EVO_HUI_TWITCH_HOME_VIEW_X && kView == EVO_HUI_TWITCH_HOME_VIEW_W &&
                  kGap == EVO_HUI_TWITCH_HOME_GAP,
              "Home's view must match EVO_HUI_TWITCH_HOME_*");
static_assert(NavRail::kWidth - 40.0f == EVO_HUI_TWITCH_HOME_DRAWN_LEFT, "cards are drawn from DRAWN_LEFT");
static_assert(kRecentW == kCatW, "Recently watched sits on the category columns");

std::string str(const char *s)
{
    return std::string(s ? s : "");
}

std::string fit(const ui::FontRef &font, const std::string &text, float size, float width)
{
    return font.font->fit(text, size, width);
}

/* Baselines of the hero's text, bottom up from the Watch button. */
struct HeroText
{
    float pill_y, name_y, title_y, game_y;
};
HeroText hero_text(int title_lines, bool has_game)
{
    HeroText t;
    t.game_y = kHeroButton.y - 44.0f;
    const float last = has_game ? t.game_y - 50.0f : t.game_y;
    t.title_y = last - kHeroTitleLead * static_cast<float>(std::max(title_lines, 1) - 1);
    t.name_y = t.title_y - 66.0f;
    t.pill_y = t.name_y - 106.0f;
    return t;
}

/* A channel watched before: its avatar in the circle `r` (it grows with
 * m.focus), ringed in Twitch's red while it is live, its name centred under
 * it and, when live, the viewers. */
void draw_recent_tile(DrawList &list, const Context &ctx, const TwitchChannel &c, const CardMotion &m, const Rect &r,
                      bool focused)
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    const bool live = c.viewers >= 0;
    const float round = r.w * 0.5f;
    const float grow = tween::lerp(1.0f, tw::kFocusScale, m.focus.value);
    list.push_transform(grow, r.cx(), r.cy(), 0, 0);
    list.shadow({r.x, r.y + 10, r.w, r.h}, round, 18, Color::rgb(0x000000, 0.25f + 0.3f * m.focus.value));
    const std::uint32_t tex = twitch_texture(ctx, c.avatar);
    if (m.art_in < 1.0f)
        draw_skeleton(list, ctx, r, round);
    if (tex)
        list.image(tex, r, cover_uv(c.avatar.w, c.avatar.h, r.w, r.h), m.tint().with_alpha(m.art_in), round);
    /* Tight to the picture, so the focus ring outside it leaves it showing. */
    if (live)
        list.bordered_rect(r.inset(-3.0f), round + 3.0f, kClear, 3.0f, twitch_live_red());
    else
        list.bordered_rect(r, round, kClear, 1.5f, kWhite.with_alpha(0.08f));
    list.pop_transform();

    CardLines &l = c.lines;
    const float width = kRecentW - 8.0f;
    if (l.fonts != &ctx.fonts || l.width != width || l.rev != c.rev)
    {
        l.fonts = &ctx.fonts;
        l.width = width;
        l.rev = c.rev;
        const std::string name = drawable(fonts.semibold, c.name);
        l.title = fonts.semibold.font->fit(name.empty() ? c.login : name, 26, width);
        l.sub = live ? fonts.regular.font->fit(format_viewers(c.viewers) + " watching", 24, width - 18) : "";
        l.chip_w = live ? fonts.regular.measure(l.sub, 24) + 18 : 0.0f;
    }
    const float cx = r.cx();
    const float name_y = r.y + r.h + tw::kTitleGap;
    ui::text(list, fonts.semibold, l.title, cx, name_y, 26, focused ? p.text : p.text.with_alpha(0.9f), Align::center);
    if (live)
    {
        const float sub_y = name_y + tw::kSubGap;
        const float x = cx - l.chip_w * 0.5f;
        list.circle(x + 5, sub_y - 8, 5, twitch_live_red());
        ui::text(list, fonts.regular, l.sub, x + 18, sub_y, 24, p.text_muted);
    }
}

} // namespace

/* -------------------------------------------------------------------- state */

void TwitchHomeScreen::set(const evo_hui_twitch_home_params_t &p)
{
    streams_status_ = p.streams_status;
    streams_error_ = str(p.streams_error);
    const int sn = std::clamp(p.stream_count, 0, EVO_HUI_TWITCH_HOME_STREAMS);
    streams_.resize(static_cast<std::size_t>(sn));
    for (int i = 0; i < sn; ++i)
    {
        TwitchStream &s = streams_[static_cast<std::size_t>(i)];
        if (assign(s, p.streams[i]))
            stream_motion_[i].art_in = 0.0f; /* another channel in this slot */
        stream_motion_[i].track(s.thumb);
    }
    hero_art_ = twitch_picture(p.hero_art);
    hero_motion_.track(hero_art_.px ? hero_art_ : (sn > 0 ? streams_[0].thumb : TwitchPicture{}));

    categories_status_ = p.categories_status;
    categories_error_ = str(p.categories_error);
    const int cn = std::clamp(p.category_count, 0, EVO_HUI_TWITCH_HOME_CATEGORIES);
    categories_.resize(static_cast<std::size_t>(cn));
    for (int i = 0; i < cn; ++i)
    {
        TwitchCategory &c = categories_[static_cast<std::size_t>(i)];
        if (assign(c, p.categories[i]))
            category_motion_[i].art_in = 0.0f;
        category_motion_[i].track(c.art);
    }

    /* Videos to continue: like the followed row, it keeps its cards while it
     * goes away. */
    continue_shown_ = p.continue_shown != 0 && p.continue_count > 0;
    if (continue_shown_)
    {
        const int vn = std::clamp(p.continue_count, 0, EVO_HUI_TWITCH_HOME_CONTINUE);
        continue_.resize(static_cast<std::size_t>(vn));
        for (int i = 0; i < vn; ++i)
        {
            TwitchVideo &v = continue_[static_cast<std::size_t>(i)];
            if (assign(v, p.continue_items[i]))
                continue_motion_[i].art_in = 0.0f;
            continue_motion_[i].track(v.thumb);
        }
    }

    /* Channels watched before: likewise. */
    recent_shown_ = p.recent_shown != 0 && p.recent_count > 0;
    if (recent_shown_)
    {
        const int rn = std::clamp(p.recent_count, 0, EVO_HUI_TWITCH_HOME_RECENT);
        recent_.resize(static_cast<std::size_t>(rn));
        for (int i = 0; i < rn; ++i)
        {
            TwitchChannel &c = recent_[static_cast<std::size_t>(i)];
            if (assign(c, p.recent[i]))
                recent_motion_[i].art_in = 0.0f;
            recent_motion_[i].track(c.avatar);
        }
    }

    /* Followed channels live: while the row goes away, it keeps the cards it
     * had, so it can fade out with them. */
    followed_shown_ = p.followed_shown != 0 && p.followed_count > 0;
    if (followed_shown_)
    {
        const int fn = std::clamp(p.followed_count, 0, EVO_HUI_TWITCH_HOME_FOLLOWED);
        followed_.resize(static_cast<std::size_t>(fn));
        for (int i = 0; i < fn; ++i)
        {
            TwitchStream &s = followed_[static_cast<std::size_t>(i)];
            if (assign(s, p.followed[i]))
                followed_motion_[i].art_in = 0.0f;
            followed_motion_[i].track(s.thumb);
        }
    }

    focus_row_ = p.focus_row;
    rail_focused_ = p.rail_focused != 0;
    stream_col_ = std::clamp(p.stream_col, 0, std::max(0, row_count() - 1));
    category_col_ = std::clamp(p.category_col, 0, std::max(0, cn - 1));
    followed_col_ = std::clamp(p.followed_col, 0, std::max(0, followed_count() - 1));
    continue_col_ = std::clamp(p.continue_col, 0, std::max(0, continue_count() - 1));
    recent_col_ = std::clamp(p.recent_col, 0, std::max(0, recent_count() - 1));
    starting_ = str(p.starting_login);
    starting_id_ = str(p.starting_id);
}

void TwitchHomeScreen::enter()
{
    age_ = 0.0f;
    ring_snapped_ = false;
    ring_alpha_.snap(0.0f);
    followed_in_.snap(followed_shown_ ? 1.0f : 0.0f);
    continue_in_.snap(continue_shown_ ? 1.0f : 0.0f);
    recent_in_.snap(recent_shown_ ? 1.0f : 0.0f);
    ring_round_.snap(focus_row_ == EVO_HUI_TWITCH_ROW_RECENT ? 1.0f : 0.0f);
    page_.snap(page_target());
    /* set() ran first: rows that already have their data show it at once and
     * only take part in the entrance. */
    streams_in_.snap(streams_status_ == EVO_HUI_TWITCH_READY && !streams_.empty() ? 1.0f : 0.0f);
    categories_in_.snap(categories_status_ == EVO_HUI_TWITCH_READY && !categories_.empty() ? 1.0f : 0.0f);
}

float TwitchHomeScreen::recent_shift() const
{
    return kRowPitch * followed_in_.value;
}

float TwitchHomeScreen::recent_shift_target() const
{
    return followed_shown_ ? kRowPitch : 0.0f;
}

float TwitchHomeScreen::continue_shift() const
{
    return recent_shift() + kRecentPitch * recent_in_.value;
}

float TwitchHomeScreen::continue_shift_target() const
{
    return recent_shift_target() + (recent_shown_ ? kRecentPitch : 0.0f);
}

float TwitchHomeScreen::shift() const
{
    return continue_shift() + kRowPitch * continue_in_.value;
}

float TwitchHomeScreen::shift_target() const
{
    return continue_shift_target() + (continue_shown_ ? kRowPitch : 0.0f);
}

float TwitchHomeScreen::row_title_target(int row) const
{
    switch (row)
    {
    case EVO_HUI_TWITCH_ROW_RECENT:
        return kRow1Title + recent_shift_target();
    case EVO_HUI_TWITCH_ROW_CONTINUE:
        return kRow1Title + continue_shift_target();
    case EVO_HUI_TWITCH_ROW_STREAMS:
        return kRow1Title + shift_target();
    case EVO_HUI_TWITCH_ROW_CATEGORIES:
        return kRow2Title + shift_target();
    default:
        return kRow1Title; /* Following */
    }
}

float TwitchHomeScreen::row_bottom_target(int row) const
{
    const float title = row_title_target(row);
    if (row == EVO_HUI_TWITCH_ROW_RECENT)
        return title + kRecentAvatarTop + kRecentAvatar + tw::kTextH;
    if (row == EVO_HUI_TWITCH_ROW_CATEGORIES)
        return title + kCardsTop + kCatH + tw::kCatTextH; /* the names on two lines */
    return title + kCardsTop + kStreamH + tw::kTextH;
}

Rect TwitchHomeScreen::followed_rect(int col) const
{
    return {kLeft + static_cast<float>(col) * (kStreamW + kGap) - followed_scroll_.offset(), kRow1Top, kStreamW,
            kStreamH};
}

Rect TwitchHomeScreen::recent_rect(int col) const
{
    const float x = kLeft + static_cast<float>(col) * (kRecentW + kGap) - recent_scroll_.offset();
    return {x + (kRecentW - kRecentAvatar) * 0.5f, kRow1Title + recent_shift() + kRecentAvatarTop, kRecentAvatar,
            kRecentAvatar};
}

Rect TwitchHomeScreen::continue_rect(int col) const
{
    return {kLeft + static_cast<float>(col) * (kStreamW + kGap) - continue_scroll_.offset(),
            kRow1Top + continue_shift(), kStreamW, kStreamH};
}

Rect TwitchHomeScreen::stream_rect(int col) const
{
    return {kLeft + static_cast<float>(col) * (kStreamW + kGap) - stream_scroll_.offset(), kRow1Top + shift(),
            kStreamW, kStreamH};
}

Rect TwitchHomeScreen::category_rect(int col) const
{
    return {kLeft + static_cast<float>(col) * (kCatW + kGap) - category_scroll_.offset(), kRow2Top + shift(), kCatW,
            kCatH};
}

float TwitchHomeScreen::page_target() const
{
    /* The first row under the hero shows with it. A row further down brings
     * itself and the row above it to the middle of what the hint band
     * leaves, with as much room above them as below. */
    const int order[] = {EVO_HUI_TWITCH_ROW_FOLLOWED, EVO_HUI_TWITCH_ROW_RECENT, EVO_HUI_TWITCH_ROW_CONTINUE,
                         EVO_HUI_TWITCH_ROW_STREAMS, EVO_HUI_TWITCH_ROW_CATEGORIES};
    auto shown = [&](int row) {
        return row == EVO_HUI_TWITCH_ROW_FOLLOWED   ? followed_shown_
               : row == EVO_HUI_TWITCH_ROW_RECENT   ? recent_shown_
               : row == EVO_HUI_TWITCH_ROW_CONTINUE ? continue_shown_
                                                     : true;
    };
    int above = -1;
    for (const int row : order)
    {
        if (row == focus_row_)
            break;
        if (shown(row))
            above = row;
    }
    if (focus_row_ == EVO_HUI_TWITCH_ROW_HERO || above < 0)
        return 0.0f;
    const float top = row_title_target(above) - kTitleCap;
    const float bottom = row_bottom_target(focus_row_);
    return top - (kHintBandTop - (bottom - top)) * 0.5f;
}

Rect TwitchHomeScreen::focus_target() const
{
    /* Where the cursor lands once the rows and the page stop, not where it is
     * now: the ring would otherwise trail the scroll. Nowhere while the rail
     * has the cursor (the page stays where it was). */
    if (rail_focused_)
        return {};
    const float page = page_target();
    if (focus_row_ == EVO_HUI_TWITCH_ROW_HERO && !streams_.empty())
        return {kHeroButton.x, kHeroButton.y - page, kHeroButton.w, kHeroButton.h};
    if (focus_row_ == EVO_HUI_TWITCH_ROW_FOLLOWED && followed_count() > 0)
    {
        Rect r = followed_rect(followed_col_);
        r.x += followed_scroll_.offset() - followed_scroll_.position.target;
        r.y -= page;
        return focus_ring_rect(r);
    }
    if (focus_row_ == EVO_HUI_TWITCH_ROW_RECENT && recent_count() > 0)
    {
        Rect r = recent_rect(recent_col_);
        r.x += recent_scroll_.offset() - recent_scroll_.position.target;
        r.y += recent_shift_target() - recent_shift() - page;
        return focus_ring_rect(r);
    }
    if (focus_row_ == EVO_HUI_TWITCH_ROW_CONTINUE && continue_count() > 0)
    {
        Rect r = continue_rect(continue_col_);
        r.x += continue_scroll_.offset() - continue_scroll_.position.target;
        r.y += continue_shift_target() - continue_shift() - page;
        return focus_ring_rect(r);
    }
    /* The rows under the optional ones: where they sit once those have slid
     * in or out. */
    const float settle = shift_target() - shift();
    if (focus_row_ == EVO_HUI_TWITCH_ROW_STREAMS && row_count() > 0)
    {
        Rect r = stream_rect(stream_col_);
        r.x += stream_scroll_.offset() - stream_scroll_.position.target;
        r.y += settle - page;
        return focus_ring_rect(r);
    }
    if (focus_row_ == EVO_HUI_TWITCH_ROW_CATEGORIES && !categories_.empty())
    {
        Rect r = category_rect(category_col_);
        r.x += category_scroll_.offset() - category_scroll_.position.target;
        r.y += settle - page;
        return focus_ring_rect(r);
    }
    return {};
}

void TwitchHomeScreen::update(float dt)
{
    age_ += dt;

    const int fn = followed_count();
    if (fn > 0)
    {
        const float start = static_cast<float>(followed_col_) * (kStreamW + kGap);
        const float limit = static_cast<float>(fn) * (kStreamW + kGap) - kGap;
        followed_scroll_.reveal(start, start + kStreamW, kView, kStreamW * EVO_HUI_TWITCH_HOME_STREAM_MARGIN, limit);
    }
    followed_scroll_.update(dt, 12.0f);
    followed_in_.target = followed_shown_ ? 1.0f : 0.0f;
    followed_in_.update(dt, 10.0f);

    const int an = recent_count();
    if (an > 0)
    {
        const float start = static_cast<float>(recent_col_) * (kRecentW + kGap);
        const float limit = static_cast<float>(an) * (kRecentW + kGap) - kGap;
        recent_scroll_.reveal(start, start + kRecentW, kView, kRecentW * EVO_HUI_TWITCH_HOME_RECENT_MARGIN, limit);
    }
    recent_scroll_.update(dt, 12.0f);
    recent_in_.target = recent_shown_ ? 1.0f : 0.0f;
    recent_in_.update(dt, 10.0f);

    const int vn = continue_count();
    if (vn > 0)
    {
        const float start = static_cast<float>(continue_col_) * (kStreamW + kGap);
        const float limit = static_cast<float>(vn) * (kStreamW + kGap) - kGap;
        continue_scroll_.reveal(start, start + kStreamW, kView, kStreamW * EVO_HUI_TWITCH_HOME_STREAM_MARGIN, limit);
    }
    continue_scroll_.update(dt, 12.0f);
    continue_in_.target = continue_shown_ ? 1.0f : 0.0f;
    continue_in_.update(dt, 10.0f);

    const int rn = row_count();
    if (rn > 0)
    {
        const float start = static_cast<float>(stream_col_) * (kStreamW + kGap);
        const float limit = static_cast<float>(rn) * (kStreamW + kGap) - kGap;
        stream_scroll_.reveal(start, start + kStreamW, kView, kStreamW * EVO_HUI_TWITCH_HOME_STREAM_MARGIN, limit);
    }
    const int cn = static_cast<int>(categories_.size());
    if (cn > 0)
    {
        const float start = static_cast<float>(category_col_) * (kCatW + kGap);
        const float limit = static_cast<float>(cn) * (kCatW + kGap) - kGap;
        category_scroll_.reveal(start, start + kCatW, kView, kCatW * EVO_HUI_TWITCH_HOME_CATEGORY_MARGIN, limit);
    }
    stream_scroll_.update(dt, 12.0f);
    category_scroll_.update(dt, 12.0f);
    page_.target = page_target();
    page_.update(dt, 10.0f);

    streams_in_.target = (streams_status_ == EVO_HUI_TWITCH_READY && !streams_.empty()) ? 1.0f : 0.0f;
    streams_in_.update(dt, 9.0f);
    categories_in_.target = (categories_status_ == EVO_HUI_TWITCH_READY && !categories_.empty()) ? 1.0f : 0.0f;
    categories_in_.update(dt, 9.0f);

    const Rect target = focus_target();
    const bool on_card = target.w > 0.0f && focus_row_ != EVO_HUI_TWITCH_ROW_HERO;
    const float round = focus_row_ == EVO_HUI_TWITCH_ROW_RECENT ? 1.0f : 0.0f;
    if (!ring_snapped_ && target.w > 0.0f)
    {
        ring_.snap(target);
        ring_round_.snap(round);
        followed_scroll_.position.snap(followed_scroll_.position.target);
        recent_scroll_.position.snap(recent_scroll_.position.target);
        continue_scroll_.position.snap(continue_scroll_.position.target);
        stream_scroll_.position.snap(stream_scroll_.position.target);
        category_scroll_.position.snap(category_scroll_.position.target);
        page_.snap(page_.target);
        ring_snapped_ = true;
    }
    if (target.w > 0.0f)
        ring_.target(target);
    ring_.update(dt, 20.0f);
    ring_round_.target = round;
    ring_round_.update(dt, 20.0f);
    ring_alpha_.target = on_card ? 1.0f : 0.0f;
    ring_alpha_.update(dt, 14.0f);
    hero_focus_.target = (!rail_focused_ && focus_row_ == EVO_HUI_TWITCH_ROW_HERO && !streams_.empty()) ? 1.0f : 0.0f;
    hero_focus_.update(dt, 16.0f);

    hero_motion_.step(dt, false, false);
    const bool on_streams = !rail_focused_ && focus_row_ == EVO_HUI_TWITCH_ROW_STREAMS;
    const bool on_categories = !rail_focused_ && focus_row_ == EVO_HUI_TWITCH_ROW_CATEGORIES;
    const bool on_followed = !rail_focused_ && focus_row_ == EVO_HUI_TWITCH_ROW_FOLLOWED;
    for (int i = 0; i < EVO_HUI_TWITCH_HOME_FOLLOWED; ++i)
        followed_motion_[i].step(dt, on_followed, on_followed && i == followed_col_);
    const bool on_recent = !rail_focused_ && focus_row_ == EVO_HUI_TWITCH_ROW_RECENT;
    for (int i = 0; i < EVO_HUI_TWITCH_HOME_RECENT; ++i)
        recent_motion_[i].step(dt, on_recent, on_recent && i == recent_col_);
    const bool on_continue = !rail_focused_ && focus_row_ == EVO_HUI_TWITCH_ROW_CONTINUE;
    for (int i = 0; i < EVO_HUI_TWITCH_HOME_CONTINUE; ++i)
        continue_motion_[i].step(dt, on_continue, on_continue && i == continue_col_);
    for (int i = 0; i < EVO_HUI_TWITCH_HOME_STREAMS; ++i)
        stream_motion_[i].step(dt, on_streams, on_streams && i == stream_col_ + 1);
    for (int i = 0; i < EVO_HUI_TWITCH_HOME_CATEGORIES; ++i)
        category_motion_[i].step(dt, on_categories, on_categories && i == category_col_);
}

/* --------------------------------------------------------------------- hero */

void TwitchHomeScreen::draw_hero_skeleton(DrawList &list, const Context &ctx, float alpha) const
{
    const HeroText t = hero_text(2, true);
    draw_skeleton(list, ctx, kHeroArt, 16, alpha);
    draw_skeleton(list, ctx, {kLeft, t.pill_y, 72, 29}, 6, alpha);
    draw_skeleton(list, ctx, {kLeft + 88, t.pill_y + 4, 180, 22}, 8, alpha);
    draw_skeleton(list, ctx, {kLeft, t.name_y - 50, 420, 56}, 10, alpha);
    draw_skeleton(list, ctx, {kLeft, t.title_y - 24, 620, 28}, 8, alpha);
    draw_skeleton(list, ctx, {kLeft, t.title_y + kHeroTitleLead - 24, 460, 28}, 8, alpha);
    draw_skeleton(list, ctx, {kLeft, t.game_y - 22, 220, 26}, 8, alpha);
    draw_skeleton(list, ctx, kHeroButton, 32, alpha);
}

void TwitchHomeScreen::draw_hero(DrawList &list, const Context &ctx, float alpha, std::uint32_t tex) const
{
    if (streams_.empty() || alpha <= 0.01f)
        return;
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    const TwitchStream &s = streams_[0];
    list.push_opacity(alpha);

    /* The picture: the large one when it has arrived, the card's until then. */
    const Rect art = kHeroArt;
    list.shadow({art.x, art.y + 28, art.w, art.h}, 24, 48, Color::rgb(0x000000, 0.55f));
    const TwitchPicture &pic = hero_art_.px ? hero_art_ : s.thumb;
    if (hero_motion_.art_in < 1.0f)
        draw_skeleton(list, ctx, art, 16);
    if (tex)
        list.image(tex, art, cover_uv(pic.w, pic.h, art.w, art.h), kWhite.with_alpha(hero_motion_.art_in), 16);
    /* The picture's left edge melts into the page, so the text beside it
     * reads as part of one block. */
    list.gradient_rect_h({art.x, art.y, art.w * 0.45f, art.h}, 16, p.bg_top.with_alpha(0.86f),
                         p.bg_top.with_alpha(0.0f));
    list.gradient_rect({art.x, art.y + art.h * 0.6f, art.w, art.h * 0.4f}, 16, kClear,
                       Color::rgb(0x000000, 0.32f));
    list.bordered_rect(art, 16, kClear, 1.5f, kWhite.with_alpha(0.08f));

    /* Who, what, where. */
    const float x = kLeft;
    HeroLines &h = hero_lines_;
    if (h.fonts != &ctx.fonts || h.rev != s.rev || h.login != s.login)
    {
        h.fonts = &ctx.fonts;
        h.rev = s.rev;
        h.login = s.login;
        h.title = fit_lines(fonts.regular, drawable(fonts.regular, s.title), kHeroTitleSize, kHeroTextW, 2);
        std::string name = drawable(fonts.display, s.name);
        h.name = fit(fonts.display, name.empty() ? s.login : name, 68, kHeroTextW);
        h.game = fit(fonts.semibold, drawable(fonts.semibold, s.game), 26, kHeroTextW);
        h.watching = format_viewers(s.viewers) + " watching";
    }
    const HeroText t = hero_text(static_cast<int>(h.title.size()), !h.game.empty());
    const float pill_w = draw_live_pill(list, ctx, x, t.pill_y, 18);
    ui::text(list, fonts.semibold, h.watching, x + pill_w + 16, t.pill_y + 23, 24, p.text_muted);

    ui::text(list, fonts.display, h.name, x - 3, t.name_y, 68, p.text);
    for (std::size_t i = 0; i < h.title.size(); ++i)
        ui::text(list, fonts.regular, h.title[i], x, t.title_y + kHeroTitleLead * static_cast<float>(i),
                 kHeroTitleSize, p.text.with_alpha(0.86f));
    if (!h.game.empty())
        ui::text(list, fonts.semibold, h.game, x, t.game_y, 26, p.accent_alt);

    /* Watch live: white with dark ink when it has the cursor, as on EVO's home. */
    draw_watch_button(list, ctx, kHeroButton, hero_focus_.value, starting_ == s.login);
    list.pop_opacity();
}

/* --------------------------------------------------------------------- rows */

void TwitchHomeScreen::draw_followed(DrawList &list, const Context &ctx, float alpha) const
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    const int n = static_cast<int>(followed_.size());
    if (n == 0 || alpha <= 0.01f)
        return;
    list.push_opacity(alpha);
    ui::text(list, fonts.semibold, "Following \xC2\xB7 live now", kLeft, kRow1Title, 30, p.text);
    const bool on_row = !rail_focused_ && focus_row_ == EVO_HUI_TWITCH_ROW_FOLLOWED && followed_count() > 0;
    if (on_row)
        draw_row_counter(list, ctx, kRow1Title, followed_col_ + 1, n);
    /* Focused card last, so its growth goes over its neighbours. */
    int focused = -1;
    for (int col = 0; col < n; ++col)
    {
        const Rect r = followed_rect(col);
        if (r.x > EVO_HUI_TWITCH_HOME_DRAWN_RIGHT || r.x + r.w < EVO_HUI_TWITCH_HOME_DRAWN_LEFT)
            continue;
        if (on_row && col == followed_col_)
        {
            focused = col;
            continue;
        }
        const TwitchStream &s = followed_[static_cast<std::size_t>(col)];
        draw_stream_card(list, ctx, s, followed_motion_[col], r, false, starting_ == s.login);
    }
    if (focused >= 0)
    {
        const TwitchStream &s = followed_[static_cast<std::size_t>(focused)];
        draw_stream_card(list, ctx, s, followed_motion_[focused], followed_rect(focused), true, starting_ == s.login);
    }
    list.pop_opacity();
}

void TwitchHomeScreen::draw_recent(DrawList &list, const Context &ctx, float alpha) const
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    const int n = static_cast<int>(recent_.size());
    if (n == 0 || alpha <= 0.01f)
        return;
    list.push_opacity(alpha);
    const float title_y = kRow1Title + recent_shift();
    ui::text(list, fonts.semibold, "Recently watched", kLeft, title_y, 30, p.text);
    const bool on_row = !rail_focused_ && focus_row_ == EVO_HUI_TWITCH_ROW_RECENT && recent_count() > 0;
    if (on_row)
        draw_row_counter(list, ctx, title_y, recent_col_ + 1, n);
    /* Focused tile last, so its growth goes over its neighbours. */
    int focused = -1;
    for (int col = 0; col < n; ++col)
    {
        /* A tile whose circle has gone behind the rail is left out: only
         * the end of its lines would show, at the rail's edge. */
        const Rect r = recent_rect(col);
        if (r.x > EVO_HUI_TWITCH_HOME_DRAWN_RIGHT || r.x + r.w < NavRail::kWidth)
            continue;
        if (on_row && col == recent_col_)
        {
            focused = col;
            continue;
        }
        draw_recent_tile(list, ctx, recent_[static_cast<std::size_t>(col)], recent_motion_[col], r, false);
    }
    if (focused >= 0)
        draw_recent_tile(list, ctx, recent_[static_cast<std::size_t>(focused)], recent_motion_[focused],
                         recent_rect(focused), true);
    list.pop_opacity();
}

void TwitchHomeScreen::draw_continue(DrawList &list, const Context &ctx, float alpha) const
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    const int n = static_cast<int>(continue_.size());
    if (n == 0 || alpha <= 0.01f)
        return;
    list.push_opacity(alpha);
    const float title_y = kRow1Title + continue_shift();
    ui::text(list, fonts.semibold, "Continue watching", kLeft, title_y, 30, p.text);
    const bool on_row = !rail_focused_ && focus_row_ == EVO_HUI_TWITCH_ROW_CONTINUE && continue_count() > 0;
    if (on_row)
        draw_row_counter(list, ctx, title_y, continue_col_ + 1, n);
    /* Focused card last, so its growth goes over its neighbours. */
    int focused = -1;
    for (int col = 0; col < n; ++col)
    {
        const Rect r = continue_rect(col);
        if (r.x > EVO_HUI_TWITCH_HOME_DRAWN_RIGHT || r.x + r.w < EVO_HUI_TWITCH_HOME_DRAWN_LEFT)
            continue;
        if (on_row && col == continue_col_)
        {
            focused = col;
            continue;
        }
        const TwitchVideo &v = continue_[static_cast<std::size_t>(col)];
        draw_video_card(list, ctx, v, continue_motion_[col], r, false, starting_id_ == v.id);
    }
    if (focused >= 0)
    {
        const TwitchVideo &v = continue_[static_cast<std::size_t>(focused)];
        draw_video_card(list, ctx, v, continue_motion_[focused], continue_rect(focused), true,
                        starting_id_ == v.id);
    }
    list.pop_opacity();
}

void TwitchHomeScreen::draw_streams(DrawList &list, const Context &ctx, float alpha) const
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    list.push_opacity(alpha);
    const float title_y = kRow1Title + shift();
    ui::text(list, fonts.semibold, "Popular now", kLeft, title_y, 30, p.text);
    const int rn = row_count();
    if (!rail_focused_ && focus_row_ == EVO_HUI_TWITCH_ROW_STREAMS && rn > 0)
        draw_row_counter(list, ctx, title_y, stream_col_ + 1, rn);

    if (rn == 0 && streams_status_ != EVO_HUI_TWITCH_LOADING)
    {
        /* The hero has the only stream there is. */
        draw_row_note(list, ctx, "Popular now", title_y, "Nobody else is live right now", "to refresh");
        list.pop_opacity();
        return;
    }
    const float in = streams_in_.value;
    if (in < 0.99f && streams_status_ != EVO_HUI_TWITCH_ERROR)
    {
        for (int i = 0; i < 6; ++i)
        {
            const Rect r{kLeft + static_cast<float>(i) * (kStreamW + kGap), kRow1Top + shift(), kStreamW, kStreamH};
            draw_card_skeleton(list, ctx, r, 0.82f, 0.56f, 1.0f - in);
        }
    }
    if (in > 0.01f)
    {
        list.push_opacity(in);
        /* Focused card last, so its growth goes over its neighbours. */
        int focused = -1;
        for (int col = 0; col < rn; ++col)
        {
            const Rect r = stream_rect(col);
            if (r.x > EVO_HUI_TWITCH_HOME_DRAWN_RIGHT || r.x + r.w < EVO_HUI_TWITCH_HOME_DRAWN_LEFT)
                continue;
            const bool f = !rail_focused_ && focus_row_ == EVO_HUI_TWITCH_ROW_STREAMS && col == stream_col_;
            if (f)
            {
                focused = col;
                continue;
            }
            const TwitchStream &s = streams_[static_cast<std::size_t>(col + 1)];
            draw_stream_card(list, ctx, s, stream_motion_[col + 1], r, false, starting_ == s.login);
        }
        if (focused >= 0)
        {
            const TwitchStream &s = streams_[static_cast<std::size_t>(focused + 1)];
            draw_stream_card(list, ctx, s, stream_motion_[focused + 1], stream_rect(focused), true,
                             starting_ == s.login);
        }
        list.pop_opacity();
    }
    list.pop_opacity();
}

void TwitchHomeScreen::draw_categories(DrawList &list, const Context &ctx, float alpha) const
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    list.push_opacity(alpha);
    const float title_y = kRow2Title + shift();
    ui::text(list, fonts.semibold, "Top categories", kLeft, title_y, 30, p.text);
    const int cn = static_cast<int>(categories_.size());
    if (!rail_focused_ && focus_row_ == EVO_HUI_TWITCH_ROW_CATEGORIES && cn > 0)
        draw_row_counter(list, ctx, title_y, category_col_ + 1, cn);

    if (cn == 0 && categories_status_ != EVO_HUI_TWITCH_LOADING)
    {
        if (categories_status_ == EVO_HUI_TWITCH_ERROR)
            draw_row_note(list, ctx, "Top categories", title_y,
                          categories_error_.empty() ? std::string("Something went wrong") : categories_error_,
                          "to try again");
        else
            draw_row_note(list, ctx, "Top categories", title_y, "No categories right now", "to refresh");
        list.pop_opacity();
        return;
    }
    const float in = categories_in_.value;
    if (in < 0.99f)
    {
        for (int i = 0; i < 8; ++i)
        {
            const Rect r{kLeft + static_cast<float>(i) * (kCatW + kGap), kRow2Top + shift(), kCatW, kCatH};
            draw_card_skeleton(list, ctx, r, 0.8f, 0.5f, 1.0f - in);
        }
    }
    if (in > 0.01f)
    {
        list.push_opacity(in);
        int focused = -1;
        for (int col = 0; col < cn; ++col)
        {
            const Rect r = category_rect(col);
            if (r.x > EVO_HUI_TWITCH_HOME_DRAWN_RIGHT || r.x + r.w < EVO_HUI_TWITCH_HOME_DRAWN_LEFT)
                continue;
            const bool f = !rail_focused_ && focus_row_ == EVO_HUI_TWITCH_ROW_CATEGORIES && col == category_col_;
            if (f)
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

void TwitchHomeScreen::draw(DrawList &list, const Context &ctx) const
{
    const float page = page_.value;

    /* The hero's picture, washed in behind everything (and drawn again as the
     * hero itself). */
    std::uint32_t hero_tex = 0;
    if (!streams_.empty())
        hero_tex = twitch_texture(ctx, hero_art_.px ? hero_art_ : streams_[0].thumb);
    draw_backdrop(list, ctx, hero_tex, 0.45f * hero_motion_.art_in);

    /* A list with nothing in it once it has landed: failed, or nobody live. */
    const bool streams_none = streams_.empty() && streams_status_ != EVO_HUI_TWITCH_LOADING;
    const bool categories_none = categories_.empty() && categories_status_ != EVO_HUI_TWITCH_LOADING;
    const bool streams_failed = streams_none && streams_status_ == EVO_HUI_TWITCH_ERROR;
    const std::string streams_text = streams_failed ? streams_error_ : std::string("Nobody is live right now");
    const char *streams_hint = streams_failed ? "to try again" : "to refresh";
    const bool rows_above = followed_count() > 0 || recent_count() > 0 || continue_count() > 0;
    if (streams_none && !rows_above && (categories_none || categories_status_ == EVO_HUI_TWITCH_LOADING))
    {
        /* Nothing to show at all: one message in the middle of the page. */
        const float in = tween::stagger(age_, 1, 0.08f, 0.6f);
        const float cx = (kLeft + kRight) * 0.5f;
        draw_message_mark(list, ctx, 352, in);
        draw_twitch_message(list, ctx, {cx - 420, 470, 840, 160}, streams_text, streams_hint, in, false);
    }
    else
    {
        list.push_clip({NavRail::kWidth, 0, ::hui::gfx::kVirtualWidth - NavRail::kWidth, ::hui::gfx::kVirtualHeight});
        list.push_transform(1.0f, 0, 0, 0, -page);

        /* Hero: fades as the page leaves it behind. */
        {
            const float in = tween::stagger(age_, 1, 0.08f, 0.6f);
            const float away = 1.0f - tween::clamp01(page / 320.0f);
            const float slide = 30.0f * (1.0f - in);
            list.push_transform(1.0f, 0, 0, slide, 0);
            if (streams_none && rows_above)
                draw_twitch_message(list, ctx, {kLeft, kHeroArt.y, kView, kHeroArt.h}, streams_text, streams_hint,
                                    in * away);
            else if (streams_none)
                draw_twitch_message(list, ctx, {kLeft, kHeroArt.y, kView, kRow1Top + kStreamH - kHeroArt.y},
                                    streams_text, streams_hint, in * away);
            else
            {
                const float ready = streams_in_.value;
                if (ready < 0.99f)
                    draw_hero_skeleton(list, ctx, in * away * (1.0f - ready));
                draw_hero(list, ctx, in * away * ready, hero_tex);
            }
            list.pop_transform();
        }

        /* The ring's light goes under the cards (a glow fills its inside).
         * A card's corners, or a circle round an avatar. */
        const Rect ring = ring_.value();
        const float radius =
            ring_round_.value < 0.001f ? -1.0f : tween::lerp(kFocusRingRadius, ring.w * 0.5f, ring_round_.value);
        list.push_transform(1.0f, 0, 0, 0, page); /* the ring is in screen space */
        draw_focus_glow(list, ctx, ring, ring_alpha_.value, radius);
        list.pop_transform();

        if (followed_in_.value > 0.01f)
        {
            const float in = tween::stagger(age_, 2, 0.09f, 0.6f);
            list.push_transform(1.0f, 0, 0, 0, 40.0f * (1.0f - in));
            draw_followed(list, ctx, in * followed_in_.value);
            list.pop_transform();
        }
        if (recent_in_.value > 0.01f)
        {
            const float in = tween::stagger(age_, 2, 0.09f, 0.6f);
            list.push_transform(1.0f, 0, 0, 0, 40.0f * (1.0f - in));
            draw_recent(list, ctx, in * recent_in_.value);
            list.pop_transform();
        }
        if (continue_in_.value > 0.01f)
        {
            const float in = tween::stagger(age_, 2, 0.09f, 0.6f);
            list.push_transform(1.0f, 0, 0, 0, 40.0f * (1.0f - in));
            draw_continue(list, ctx, in * continue_in_.value);
            list.pop_transform();
        }
        if (!streams_none)
        {
            const float in = tween::stagger(age_, 3, 0.09f, 0.6f);
            list.push_transform(1.0f, 0, 0, 0, 40.0f * (1.0f - in));
            draw_streams(list, ctx, in);
            list.pop_transform();
        }
        {
            const float in = tween::stagger(age_, 4, 0.09f, 0.6f);
            list.push_transform(1.0f, 0, 0, 0, 40.0f * (1.0f - in));
            draw_categories(list, ctx, in);
            list.pop_transform();
        }

        /* The ring itself, over the focused card. */
        list.push_transform(1.0f, 0, 0, 0, page);
        draw_focus_ring(list, ctx, ring, ring_alpha_.value, radius);
        list.pop_transform();
        list.pop_transform();
        list.pop_clip();
        draw_hint_band(list, ctx);
    }

    ui::Hint hints[3];
    int n = 0;
    if (rail_focused_)
        n = rail_hints(hints);
    else
    {
        if (focus_row_ == EVO_HUI_TWITCH_ROW_CATEGORIES && !categories_.empty())
            hints[n++] = {ui::Button::cross, "Open"};
        else if (focus_row_ == EVO_HUI_TWITCH_ROW_CONTINUE && continue_count() > 0)
        {
            hints[n++] = {ui::Button::cross, "Resume"};
            hints[n++] = {ui::Button::square, "Channel"};
        }
        else if (focus_row_ == EVO_HUI_TWITCH_ROW_RECENT && recent_count() > 0)
        {
            /* Live: Cross watches, Square opens the page; offline, both open
             * it, and Cross says so in the same word Square uses. */
            if (recent_[static_cast<std::size_t>(recent_col_)].viewers >= 0)
            {
                hints[n++] = {ui::Button::cross, "Watch"};
                hints[n++] = {ui::Button::square, "Channel"};
            }
            else
                hints[n++] = {ui::Button::cross, "Channel"};
        }
        else if ((focus_row_ == EVO_HUI_TWITCH_ROW_HERO && !streams_.empty()) ||
                 (focus_row_ == EVO_HUI_TWITCH_ROW_FOLLOWED && followed_count() > 0) ||
                 (focus_row_ == EVO_HUI_TWITCH_ROW_STREAMS && row_count() > 0))
        {
            hints[n++] = {ui::Button::cross, "Watch"};
            hints[n++] = {ui::Button::square, "Channel"};
        }
        hints[n++] = {ui::Button::triangle, "Refresh"};
    }
    draw_twitch_hints(list, ctx, hints, n, tween::stagger(age_, 6, 0.08f, 0.5f));
}

/* -------------------------------------------------------------------- focus */

bool TwitchHomeScreen::focus(FocusInfo *out) const
{
    out->focusable.clear();
    if (!streams_.empty())
        out->focusable.emplace_back("hero", "Watch " + streams_[0].name);
    for (int i = 0; i < followed_count(); ++i)
        out->focusable.emplace_back("followed-" + std::to_string(i), followed_[static_cast<std::size_t>(i)].name);
    for (int i = 0; i < recent_count(); ++i)
        out->focusable.emplace_back("recent-" + std::to_string(i), recent_[static_cast<std::size_t>(i)].name);
    for (int i = 0; i < continue_count(); ++i)
        out->focusable.emplace_back("continue-" + std::to_string(i), continue_[static_cast<std::size_t>(i)].title);
    for (int i = 0; i < row_count(); ++i)
        out->focusable.emplace_back("stream-" + std::to_string(i), streams_[static_cast<std::size_t>(i + 1)].name);
    for (std::size_t i = 0; i < categories_.size(); ++i)
        out->focusable.emplace_back("category-" + std::to_string(i), categories_[i].name);

    const float page = page_target();
    if (rail_focused_)
    {
        out->focusable.clear();
        return false;
    }
    if (focus_row_ == EVO_HUI_TWITCH_ROW_HERO && !streams_.empty())
    {
        out->id = "hero";
        out->text = "Watch " + streams_[0].name;
        out->index = out->total = 1;
        out->rect = kHeroButton;
        return true;
    }
    if (focus_row_ == EVO_HUI_TWITCH_ROW_FOLLOWED && followed_count() > 0)
    {
        const TwitchStream &s = followed_[static_cast<std::size_t>(followed_col_)];
        out->id = "followed-" + std::to_string(followed_col_);
        out->text = s.name + ": " + s.title;
        out->index = followed_col_ + 1;
        out->total = followed_count();
        Rect r = followed_rect(followed_col_);
        r.y -= page;
        out->rect = r;
        return true;
    }
    if (focus_row_ == EVO_HUI_TWITCH_ROW_RECENT && recent_count() > 0)
    {
        const TwitchChannel &c = recent_[static_cast<std::size_t>(recent_col_)];
        out->id = "recent-" + std::to_string(recent_col_);
        out->text = c.viewers >= 0 ? c.name + ", live" : c.name;
        out->index = recent_col_ + 1;
        out->total = recent_count();
        Rect r = recent_rect(recent_col_);
        r.y += recent_shift_target() - recent_shift() - page;
        out->rect = r;
        return true;
    }
    if (focus_row_ == EVO_HUI_TWITCH_ROW_CONTINUE && continue_count() > 0)
    {
        const TwitchVideo &v = continue_[static_cast<std::size_t>(continue_col_)];
        out->id = "continue-" + std::to_string(continue_col_);
        out->text = v.title;
        out->index = continue_col_ + 1;
        out->total = continue_count();
        Rect r = continue_rect(continue_col_);
        r.y += continue_shift_target() - continue_shift() - page;
        out->rect = r;
        return true;
    }
    if (focus_row_ == EVO_HUI_TWITCH_ROW_STREAMS && row_count() > 0)
    {
        const TwitchStream &s = streams_[static_cast<std::size_t>(stream_col_ + 1)];
        out->id = "stream-" + std::to_string(stream_col_);
        out->text = s.name + ": " + s.title;
        out->index = stream_col_ + 1;
        out->total = row_count();
        Rect r = stream_rect(stream_col_);
        r.y += shift_target() - shift() - page;
        out->rect = r;
        return true;
    }
    if (focus_row_ == EVO_HUI_TWITCH_ROW_CATEGORIES && !categories_.empty())
    {
        out->id = "category-" + std::to_string(category_col_);
        out->text = categories_[static_cast<std::size_t>(category_col_)].name;
        out->index = category_col_ + 1;
        out->total = static_cast<int>(categories_.size());
        Rect r = category_rect(category_col_);
        r.y += shift_target() - shift() - page;
        out->rect = r;
        return true;
    }
    out->focusable.clear();
    return false;
}

} // namespace evo::kit
