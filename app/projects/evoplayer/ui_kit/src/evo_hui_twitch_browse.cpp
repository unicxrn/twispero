/*
 * evo_hui_twitch_browse.cpp - Browse (the top categories) and one category's
 * live streams and clips, on ps5-homebrew-ui.
 *
 * Both are a header over a grid that fills the content width: box art six to
 * a row on Browse, 16:9 cards four to a row on a category. A category has
 * two tabs under its header, Live and Clips; the Clips tab has its time
 * window's chips over its grid (evo_hui_twitch_videos.cpp). The cards,
 * skeletons, ring and hints are the home screen's (evo_hui_twitch_common.cpp).
 * The page scrolls on a spring so the cursor's row sits second from the top
 * with the row above it in full (TwitchGrid::page_for); the header scrolls
 * away with it.
 *
 * core/src/screens/TwitchBrowseScreen.cpp and TwitchCategoryScreen.cpp own
 * the data and the cursor; this only draws what they hand over.
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

/* The header: a title and a line under it, as on the placeholder this
 * replaced, and the grid's first row under them. */
constexpr float kTitleY = 136.0f;
constexpr float kLineY = 184.0f;
constexpr float kGridTop = 248.0f;

/* Box art beside a category's name. */
const Rect kArt{kLeft, 72, 96, 128};
constexpr float kArtRadius = 10.0f;
constexpr float kNameX = kLeft + 96.0f + 32.0f;

/* A category's tabs under its header, the clips' chips under them, and where
 * each tab's grid starts. */
constexpr float kTabsY = 232.0f;
constexpr float kChipsY = kTabsY + tw::kTabH + tw::kTabsToChips;
constexpr float kLiveTop = kTabsY + tw::kTabH + tw::kAboveGrid;
constexpr float kClipsTop = kChipsY + tw::kChipH + tw::kAboveGrid;
const char *const kCategoryTabs[] = {"Live", "Clips"};

/* Where a message goes when there is nothing to show: the home screen's spot
 * for its own (the mark over one sentence and a hint). */
constexpr float kMarkY = 352.0f;
const Rect kMessage{(kLeft + kRight) * 0.5f - 600.0f, 470, 1200, 160};
/* A category tab's own message: a panel where its cards would be (a channel
 * page's kNoteH). */
constexpr float kTabNoteH = 200.0f;

std::string str(const char *s)
{
    return std::string(s ? s : "");
}

/* The header goes as the page leaves it. */
float header_alpha(float page)
{
    return 1.0f - tween::clamp01(page / 160.0f);
}

/* Rows come in one after another, rising a little. */
float row_in(float age, int row)
{
    return tween::stagger(age, 1 + std::min(row, 4), 0.06f, 0.5f);
}

/* A card on the page is worth drawing when some of it (lines included) is on
 * screen. */
bool on_screen(const Rect &r, float page, float text_h)
{
    const float y = r.y - page;
    return y < ::hui::gfx::kVirtualHeight && y + r.h + text_h > 0.0f;
}

/*
 * The grid itself: skeletons while it loads, then the cards, the focused one
 * last so its growth goes over its neighbours, with the ring's light under
 * them and the ring over them. Draws in page coordinates (the caller has
 * moved the page up by `page`); the ring is in screen space.
 */
template <typename DrawCard>
void draw_grid(DrawList &list, const Context &ctx, const TwitchGrid &grid, const TwitchGridMotion &motion, int count,
               int focused, float title_w, float sub_w, DrawCard &&draw_card, bool draw_ring = true,
               bool third_line = false)
{
    const float page = motion.page.value;
    const Rect ring = motion.ring.value();
    if (draw_ring)
    {
        list.push_transform(1.0f, 0, 0, 0, page);
        draw_focus_glow(list, ctx, ring, motion.ring_alpha.value);
        list.pop_transform();
    }

    const float in = motion.in.value;
    if (in < 0.99f)
    {
        /* As many rows of skeletons as fill the screen. */
        for (int i = 0; i < grid.cols * 3; ++i)
        {
            const Rect r = grid.cell(i);
            if (!on_screen(r, page, grid.text_h))
                continue;
            const float a = (1.0f - in) * row_in(motion.age, i / grid.cols);
            draw_card_skeleton(list, ctx, r, title_w, sub_w, a);
            if (third_line) /* a video card's detail */
                draw_skeleton(list, ctx, {r.x, r.y + r.h + tw::kTitleGap + 48, r.w * 0.38f, 20}, 8, a);
        }
    }
    if (in > 0.01f)
    {
        list.push_opacity(in);
        auto draw_one = [&](int i) {
            const Rect r = grid.cell(i);
            const float a = row_in(motion.age, i / grid.cols);
            list.push_opacity(a);
            list.push_transform(1.0f, 0, 0, 0, 40.0f * (1.0f - a));
            draw_card(i, r, i == focused);
            list.pop_transform();
            list.pop_opacity();
        };
        for (int i = 0; i < count; ++i)
            if (i != focused && on_screen(grid.cell(i), page, grid.text_h))
                draw_one(i);
        if (focused >= 0 && focused < count)
            draw_one(focused);
        list.pop_opacity();
    }

    if (draw_ring)
    {
        list.push_transform(1.0f, 0, 0, 0, page);
        draw_focus_ring(list, ctx, ring, motion.ring_alpha.value);
        list.pop_transform();
    }
}

/* The ring's place for card `cursor` once the page has stopped. */
Rect grid_ring(const TwitchGrid &grid, int cursor, int count)
{
    Rect r = grid.cell(cursor);
    r.y -= grid.page_for(cursor, count);
    return focus_ring_rect(r);
}

void grid_focus(FocusInfo *out, const TwitchGrid &grid, int cursor, int count)
{
    out->index = cursor + 1;
    out->total = count;
    Rect r = grid.cell(cursor);
    r.y -= grid.page_for(cursor, count);
    out->rect = r;
}

} // namespace

/* ------------------------------------------------------------------- Browse */

TwitchGrid TwitchBrowseScreen::grid()
{
    return TwitchGrid::make(EVO_HUI_TWITCH_BROWSE_COLS, 4.0f / 3.0f, kGridTop, tw::kCatRowGap, tw::kCatTextH);
}

void TwitchBrowseScreen::set(const evo_hui_twitch_browse_params_t &p)
{
    status_ = p.status;
    error_ = str(p.error);
    const int n = std::clamp(p.category_count, 0, EVO_HUI_TWITCH_BROWSE_CATEGORIES);
    categories_.resize(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
    {
        TwitchCategory &c = categories_[static_cast<std::size_t>(i)];
        if (assign(c, p.categories[i]))
            cards_[i].art_in = 0.0f; /* another category in this slot */
        cards_[i].track(c.art);
    }
    cursor_ = std::clamp(p.cursor, 0, std::max(0, n - 1));
    rail_focused_ = p.rail_focused != 0;
}

Rect TwitchBrowseScreen::ring_target() const
{
    if (rail_focused_ || categories_.empty())
        return {};
    return grid_ring(grid(), cursor_, static_cast<int>(categories_.size()));
}

void TwitchBrowseScreen::enter()
{
    motion_.enter(grid().page_for(cursor_, static_cast<int>(categories_.size())), !categories_.empty());
}

void TwitchBrowseScreen::update(float dt)
{
    const int n = static_cast<int>(categories_.size());
    motion_.update(dt, grid().page_for(cursor_, n), ring_target(), n > 0);
    const bool focused = !rail_focused_ && n > 0;
    for (int i = 0; i < EVO_HUI_TWITCH_BROWSE_CATEGORIES; ++i)
        cards_[i].step(dt, focused, focused && i == cursor_);
}

void TwitchBrowseScreen::draw(DrawList &list, const Context &ctx) const
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    const TwitchGrid g = grid();
    const int n = static_cast<int>(categories_.size());
    const float page = motion_.page.value;
    draw_backdrop(list, ctx);

    list.push_clip({NavRail::kWidth, 0, ::hui::gfx::kVirtualWidth - NavRail::kWidth, ::hui::gfx::kVirtualHeight});
    list.push_transform(1.0f, 0, 0, 0, -page);
    {
        const float in = tween::stagger(motion_.age, 0, 0.05f, 0.45f);
        list.push_opacity(in * header_alpha(page));
        const float dy = 16.0f * (1.0f - in);
        ui::text(list, fonts.display, "Browse", kLeft - 2, kTitleY + dy, 56, p.text);
        ui::text(list, fonts.regular, "Top categories right now", kLeft, kLineY + dy, 26, p.text_muted);
        list.pop_opacity();
    }

    if (n == 0 && status_ != EVO_HUI_TWITCH_LOADING)
    {
        const float in = tween::stagger(motion_.age, 1, 0.08f, 0.6f);
        draw_message_mark(list, ctx, kMarkY, in);
        if (status_ == EVO_HUI_TWITCH_ERROR)
            draw_twitch_message(list, ctx, kMessage, error_, "to try again", in, false);
        else
            draw_twitch_message(list, ctx, kMessage, "No categories right now", "to refresh", in, false);
    }
    else
    {
        const int focused = rail_focused_ ? -1 : cursor_;
        draw_grid(list, ctx, g, motion_, n, focused, 0.8f, 0.5f, [&](int i, const Rect &r, bool f) {
            draw_category_card(list, ctx, categories_[static_cast<std::size_t>(i)], cards_[i], r, f);
        });
    }
    list.pop_transform();
    list.pop_clip();
    if (n > 0 || status_ == EVO_HUI_TWITCH_LOADING)
        draw_hint_band(list, ctx);

    ui::Hint hints[3];
    int h = 0;
    if (rail_focused_)
        h = rail_hints(hints);
    else
    {
        if (n > 0)
            hints[h++] = {ui::Button::cross, "Open"};
        hints[h++] = {ui::Button::triangle, "Refresh"};
        hints[h++] = {ui::Button::circle, "Back"};
    }
    draw_twitch_hints(list, ctx, hints, h, tween::stagger(motion_.age, 6, 0.08f, 0.5f));
}

bool TwitchBrowseScreen::focus(FocusInfo *out) const
{
    out->focusable.clear();
    const int n = static_cast<int>(categories_.size());
    for (int i = 0; i < n; ++i)
        out->focusable.emplace_back("category-" + std::to_string(i), categories_[static_cast<std::size_t>(i)].name);
    if (rail_focused_ || n == 0)
    {
        out->focusable.clear();
        return false;
    }
    out->id = "category-" + std::to_string(cursor_);
    out->text = categories_[static_cast<std::size_t>(cursor_)].name;
    grid_focus(out, grid(), cursor_, n);
    return true;
}

/* ----------------------------------------------------------------- Category */

TwitchGrid TwitchCategoryScreen::grid() const
{
    if (on_clips())
        return TwitchGrid::make(EVO_HUI_TWITCH_VIDEO_COLS, 9.0f / 16.0f, kClipsTop, tw::kVideoRowGap,
                                tw::kVideoTextH);
    return TwitchGrid::make(EVO_HUI_TWITCH_CATEGORY_COLS, 9.0f / 16.0f, kLiveTop);
}

int TwitchCategoryScreen::count() const
{
    return static_cast<int>(on_clips() ? clips_.size() : streams_.size());
}

int TwitchCategoryScreen::grid_cursor() const
{
    return on_clips() ? clip_cursor_ : cursor_;
}

int TwitchCategoryScreen::list_status() const
{
    return on_clips() ? clips_status_ : status_;
}

void TwitchCategoryScreen::set(const evo_hui_twitch_category_params_t &p)
{
    if (name_ != str(p.name))
        art_motion_.art_in = 0.0f;
    name_ = str(p.name);
    viewers_ = p.viewers;
    art_ = twitch_picture(p.art);
    art_motion_.track(art_);

    status_ = p.status;
    error_ = str(p.error);
    const int n = std::clamp(p.stream_count, 0, EVO_HUI_TWITCH_CATEGORY_STREAMS);
    streams_.resize(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
    {
        TwitchStream &s = streams_[static_cast<std::size_t>(i)];
        if (assign(s, p.streams[i]))
            cards_[i].art_in = 0.0f; /* another channel in this slot */
        cards_[i].track(s.thumb);
    }
    cursor_ = std::clamp(p.cursor, 0, std::max(0, n - 1));
    rail_focused_ = p.rail_focused != 0;
    starting_ = str(p.starting_login);

    tab_ = p.tab == EVO_HUI_TWITCH_CATEGORY_TAB_CLIPS ? EVO_HUI_TWITCH_CATEGORY_TAB_CLIPS
                                                      : EVO_HUI_TWITCH_CATEGORY_TAB_LIVE;
    focus_ = p.focus;
    if (focus_ != EVO_HUI_TWITCH_FOCUS_TABS && focus_ != EVO_HUI_TWITCH_FOCUS_FILTERS)
        focus_ = EVO_HUI_TWITCH_FOCUS_GRID;
    if (focus_ == EVO_HUI_TWITCH_FOCUS_FILTERS && !on_clips())
        focus_ = EVO_HUI_TWITCH_FOCUS_TABS;
    filter_ = std::clamp(p.filter, 0, EVO_HUI_TWITCH_CLIP_FILTERS - 1);
    filter_cursor_ = std::clamp(p.filter_cursor, 0, EVO_HUI_TWITCH_CLIP_FILTERS - 1);
    clips_status_ = p.clips_status;
    clips_error_ = str(p.clips_error);
    const int cn = std::clamp(p.clip_count, 0, EVO_HUI_TWITCH_CATEGORY_CLIPS);
    clips_.resize(static_cast<std::size_t>(cn));
    for (int i = 0; i < cn; ++i)
    {
        TwitchVideo &v = clips_[static_cast<std::size_t>(i)];
        if (assign(v, p.clips[i]))
            clip_cards_[i].art_in = 0.0f; /* another clip in this slot */
        clip_cards_[i].track(v.thumb);
    }
    clip_cursor_ = std::clamp(p.clip_cursor, 0, std::max(0, cn - 1));
    starting_id_ = str(p.starting_id);
}

float TwitchCategoryScreen::page_target() const
{
    const int n = count();
    if (focus_ != EVO_HUI_TWITCH_FOCUS_GRID || n == 0)
        return 0.0f;
    return grid().page_for(grid_cursor(), n);
}

Rect TwitchCategoryScreen::ring_target() const
{
    /* On the tabs their row rings the open one itself (draw_tab_row). */
    if (rail_focused_ || focus_ == EVO_HUI_TWITCH_FOCUS_TABS)
        return {};
    if (focus_ == EVO_HUI_TWITCH_FOCUS_FILTERS)
        return chip_rect(filter_cursor_, kChipsY, kClipChipW).inset(-7.0f);
    if (count() == 0)
        return {};
    return grid_ring(grid(), grid_cursor(), count());
}

void TwitchCategoryScreen::enter()
{
    motion_.enter(page_target(), count() > 0);
    const bool pill = focus_ != EVO_HUI_TWITCH_FOCUS_GRID;
    ring_round_.snap(pill ? 1.0f : 0.0f);
    tab_line_.snap(static_cast<float>(tab_));
    tabs_focus_.snap(!rail_focused_ && focus_ == EVO_HUI_TWITCH_FOCUS_TABS ? 1.0f : 0.0f);
    shown_tab_ = tab_;
}

void TwitchCategoryScreen::update(float dt)
{
    const int n = count();
    /* Another tab, or a list emptied for another time window: no cards to
     * fade out, so its skeletons show at once. */
    if (shown_tab_ != tab_ || n == 0)
        motion_.in.snap(n > 0 ? 1.0f : 0.0f);
    shown_tab_ = tab_;
    motion_.update(dt, page_target(), ring_target(), n > 0);
    ring_round_.target = focus_ != EVO_HUI_TWITCH_FOCUS_GRID ? 1.0f : 0.0f;
    ring_round_.update(dt, 20.0f);
    tab_line_.target = static_cast<float>(tab_);
    tab_line_.update(dt, 18.0f);
    tabs_focus_.target = !rail_focused_ && focus_ == EVO_HUI_TWITCH_FOCUS_TABS ? 1.0f : 0.0f;
    tabs_focus_.update(dt, 16.0f);

    const bool in_grid = !rail_focused_ && focus_ == EVO_HUI_TWITCH_FOCUS_GRID && n > 0;
    const bool live = in_grid && !on_clips(), clips = in_grid && on_clips();
    for (int i = 0; i < EVO_HUI_TWITCH_CATEGORY_STREAMS; ++i)
        cards_[i].step(dt, live, live && i == cursor_);
    for (int i = 0; i < EVO_HUI_TWITCH_CATEGORY_CLIPS; ++i)
        clip_cards_[i].step(dt, clips, clips && i == clip_cursor_);
    art_motion_.step(dt, false, false);
}

void TwitchCategoryScreen::draw_header(DrawList &list, const Context &ctx, float page) const
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    const float in = tween::stagger(motion_.age, 0, 0.05f, 0.45f);
    list.push_opacity(in * header_alpha(page));
    list.push_transform(1.0f, 0, 0, 0, 16.0f * (1.0f - in));

    list.shadow({kArt.x, kArt.y + 8, kArt.w, kArt.h}, kArtRadius, 16, Color::rgb(0x000000, 0.4f));
    const std::uint32_t tex = twitch_texture(ctx, art_);
    if (art_motion_.art_in < 1.0f)
        draw_skeleton(list, ctx, kArt, kArtRadius);
    if (tex)
        list.image(tex, kArt, cover_uv(art_.w, art_.h, kArt.w, kArt.h), kWhite.with_alpha(art_motion_.art_in),
                   kArtRadius);
    list.bordered_rect(kArt, kArtRadius, kClear, 1.5f, kWhite.with_alpha(0.08f));

    ui::text(list, fonts.display, name_fit_.get(fonts.display, name_, 56, kRight - kNameX), kNameX - 2, kTitleY, 56,
             p.text);
    if (viewers_ >= 0)
        ui::text(list, fonts.regular, format_viewers(viewers_) + " watching", kNameX, kLineY, 26, p.text_muted);
    list.pop_transform();
    list.pop_opacity();

    /* The tabs and the clips' chips leave with the header. */
    const float bar = tween::stagger(motion_.age, 1, 0.05f, 0.45f) * header_alpha(page);
    draw_tab_row(list, ctx, kCategoryTabs, 2, tab_, tab_line_.value, kTabsY, bar, tabs_focus_.value);
    if (on_clips())
        draw_chip_row(list, ctx, kClipFilterLabels, EVO_HUI_TWITCH_CLIP_FILTERS, filter_, kChipsY, kClipChipW, bar);
}

void TwitchCategoryScreen::draw_empty(DrawList &list, const Context &ctx) const
{
    const ui::Fonts &fonts = ctx.fonts;
    const float in = tween::stagger(motion_.age, 1, 0.08f, 0.6f);
    /* A tab with nothing in it says so where its cards would be, in a panel,
     * as a channel page's Videos and Clips do. */
    const Rect message{kLeft, grid().top, tw::kView, kTabNoteH};
    if (list_status() == EVO_HUI_TWITCH_ERROR)
        draw_twitch_message(list, ctx, message, on_clips() ? clips_error_ : error_, "to try again", in);
    else if (on_clips())
        draw_twitch_message(list, ctx, message, empty_clips_text(filter_), "to refresh", in);
    else
    {
        /* "Nobody is live in <name> right now", the name cut to fit. */
        const std::string before = "Nobody is live in ", after = " right now";
        const float room = message.w - fonts.display.measure(before + after, 40);
        const std::string what = before + empty_fit_.get(fonts.display, name_, 40, std::max(room, 120.0f)) + after;
        draw_twitch_message(list, ctx, message, what, "to refresh", in);
    }
}

void TwitchCategoryScreen::draw(DrawList &list, const Context &ctx) const
{
    const TwitchGrid g = grid();
    const int n = count();
    const float page = motion_.page.value;
    draw_backdrop(list, ctx);

    /* The ring: a pill on a tab or a chip, a card's corners on a card; its
     * light only under a card. Both in screen space. */
    const Rect ring = motion_.ring.value();
    const float round = ring_round_.value;
    const float radius = tween::lerp(kFocusRingRadius, ring.h * 0.5f, round);

    list.push_clip({NavRail::kWidth, 0, ::hui::gfx::kVirtualWidth - NavRail::kWidth, ::hui::gfx::kVirtualHeight});
    list.push_transform(1.0f, 0, 0, 0, -page);
    list.push_transform(1.0f, 0, 0, 0, page);
    draw_focus_glow(list, ctx, ring, motion_.ring_alpha.value * (1.0f - round), radius);
    list.pop_transform();
    draw_header(list, ctx, page);

    if (n == 0 && list_status() != EVO_HUI_TWITCH_LOADING)
        draw_empty(list, ctx);
    else
    {
        const int focused = rail_focused_ || focus_ != EVO_HUI_TWITCH_FOCUS_GRID ? -1 : grid_cursor();
        if (on_clips())
            draw_grid(
                list, ctx, g, motion_, n, focused, 0.82f, 0.56f,
                [&](int i, const Rect &r, bool f) {
                    const TwitchVideo &v = clips_[static_cast<std::size_t>(i)];
                    draw_video_card(list, ctx, v, clip_cards_[i], r, f, !starting_id_.empty() && starting_id_ == v.id);
                },
                false, true);
        else
            draw_grid(
                list, ctx, g, motion_, n, focused, 0.82f, 0.56f,
                [&](int i, const Rect &r, bool f) {
                    const TwitchStream &s = streams_[static_cast<std::size_t>(i)];
                    draw_stream_card(list, ctx, s, cards_[i], r, f, starting_ == s.login);
                },
                false);
    }
    list.push_transform(1.0f, 0, 0, 0, page);
    draw_focus_ring(list, ctx, ring, motion_.ring_alpha.value, radius);
    list.pop_transform();
    list.pop_transform();
    list.pop_clip();
    if (n > 0 || list_status() == EVO_HUI_TWITCH_LOADING)
        draw_hint_band(list, ctx);

    ui::Hint hints[4];
    int h = 0;
    if (rail_focused_)
        h = rail_hints(hints);
    else
    {
        if (focus_ == EVO_HUI_TWITCH_FOCUS_GRID && n > 0)
        {
            hints[h++] = {ui::Button::cross, "Watch"};
            hints[h++] = {ui::Button::square, "Channel"};
        }
        else if (focus_ == EVO_HUI_TWITCH_FOCUS_FILTERS && filter_cursor_ != filter_)
            hints[h++] = {ui::Button::cross, "Select"};
        hints[h++] = {ui::Button::triangle, "Refresh"};
        hints[h++] = {ui::Button::circle, "Back"};
    }
    draw_twitch_hints(list, ctx, hints, h, tween::stagger(motion_.age, 6, 0.08f, 0.5f));
}

bool TwitchCategoryScreen::focus(FocusInfo *out) const
{
    out->focusable.clear();
    out->focusable.emplace_back("tab-live", kCategoryTabs[0]);
    out->focusable.emplace_back("tab-clips", kCategoryTabs[1]);
    if (on_clips())
        for (int i = 0; i < EVO_HUI_TWITCH_CLIP_FILTERS; ++i)
            out->focusable.emplace_back("filter-" + std::to_string(i), kClipFilterLabels[i]);
    const int n = count();
    for (int i = 0; i < n; ++i)
    {
        if (on_clips())
            out->focusable.emplace_back("clip-" + std::to_string(i), clips_[static_cast<std::size_t>(i)].title);
        else
            out->focusable.emplace_back("stream-" + std::to_string(i), streams_[static_cast<std::size_t>(i)].name);
    }
    if (rail_focused_)
    {
        out->focusable.clear();
        return false;
    }
    if (focus_ == EVO_HUI_TWITCH_FOCUS_TABS)
    {
        out->id = on_clips() ? "tab-clips" : "tab-live";
        out->text = kCategoryTabs[tab_];
        out->index = tab_ + 1;
        out->total = 2;
        out->rect = tab_rect(tab_, kTabsY);
        return true;
    }
    if (focus_ == EVO_HUI_TWITCH_FOCUS_FILTERS)
    {
        out->id = "filter-" + std::to_string(filter_cursor_);
        out->text = std::string(kClipFilterLabels[filter_cursor_]) + (filter_cursor_ == filter_ ? ", selected" : "");
        out->index = filter_cursor_ + 1;
        out->total = EVO_HUI_TWITCH_CLIP_FILTERS;
        out->rect = chip_rect(filter_cursor_, kChipsY, kClipChipW);
        return true;
    }
    if (n == 0)
    {
        out->focusable.clear();
        return false;
    }
    if (on_clips())
    {
        const TwitchVideo &v = clips_[static_cast<std::size_t>(clip_cursor_)];
        out->id = "clip-" + std::to_string(clip_cursor_);
        out->text = v.detail.empty() ? v.title : v.detail + ": " + v.title;
        grid_focus(out, grid(), clip_cursor_, n);
        return true;
    }
    const TwitchStream &s = streams_[static_cast<std::size_t>(cursor_)];
    out->id = "stream-" + std::to_string(cursor_);
    out->text = s.name + ": " + s.title;
    grid_focus(out, grid(), cursor_, n);
    return true;
}

} // namespace evo::kit
