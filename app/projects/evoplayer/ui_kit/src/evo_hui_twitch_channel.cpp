/*
 * evo_hui_twitch_channel.cpp - one channel's page on ps5-homebrew-ui.
 *
 * Top to bottom: the profile (a round avatar, the name, followers and up to
 * three lines of description, laid out like a category's header, with a small
 * "Schedule" panel at its right when the channel has streams planned), the
 * live stream as Home's hero has it (LIVE, the viewers and how long it has
 * been on, the title, the category and the stream's tags on the left over
 * "Watch live", the preview on the right) when the channel is live, then the
 * Videos and
 * Clips tabs, the open tab's filter chips (Past broadcasts, Highlights,
 * Uploads; Last 24 hours, 7 days, 30 days, All time) and its cards four to a
 * row. Offline, OFFLINE sits on the followers' line and the tabs come right
 * under the profile, so the first row of videos is on screen at once.
 *
 * One ring runs through the tabs, the chips and the cards; Watch live lights
 * up instead of taking it. The page scrolls on a spring: not at all while the
 * cursor's row fits, then far enough to put the tabs at the top (the grid's
 * first row lands where a category's does), then, deeper in the grid, with
 * the cursor's row second from the top as on a category. While the page
 * loads, skeletons in the profile's and the stream's places; while a tab's
 * list loads, skeleton cards; when the page fails, the shared message in the
 * middle; when a list fails or is empty, a quiet panel where its cards go.
 *
 * core/src/screens/TwitchChannelScreen.cpp owns the data and the cursor and
 * fills evo_hui_twitch_channel_params_t; this only draws it.
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
constexpr int kCols = EVO_HUI_TWITCH_VIDEO_COLS;

/* The profile: the avatar where a category's box art sits, the name and the
 * followers on the title and line baselines of the other pages. */
const Rect kAvatar{kLeft, 72, 150, 150};
constexpr float kNameX = kLeft + 150.0f + 40.0f;
constexpr float kTitleY = 136.0f;
constexpr float kLineY = 184.0f;
constexpr float kDescY = 232.0f;
constexpr float kDescSize = 24.0f;
constexpr float kDescLead = 34.0f;
constexpr float kDescW = 1080.0f;

/* The schedule: a small panel at the profile's right, under the name's line
 * and clear of the tabs on an offline page. The description stops short of it. */
constexpr float kSchedW = 420.0f;
constexpr float kSchedX = kRight - kSchedW;
constexpr float kSchedTop = 148.0f;
constexpr float kSchedPad = 22.0f;
constexpr float kSchedEyebrowY = kSchedTop + 32.0f;
constexpr float kSchedRowY = kSchedTop + 66.0f; /* the first row's baseline */
constexpr float kSchedLead = 34.0f;
constexpr float kSchedWhenW = 118.0f; /* "In 3 days" and a gap */
constexpr float kSchedText = 22.0f;
constexpr float kDescWSched = kSchedX - 48.0f - kNameX;
static_assert(kSchedRowY + kSchedLead * (EVO_HUI_TWITCH_CHANNEL_SCHEDULE - 1) + 18.0f <= 336.0f - 30.0f,
              "the schedule ends clear of an offline page's tabs");

/* The live stream's tags: quiet pills on one line under the category. */
constexpr float kTagH = 34.0f;
constexpr float kTagText = 20.0f;
constexpr float kTagPad = 14.0f;
constexpr float kTagGap = 8.0f;
constexpr float kTagMaxW = 260.0f;

/* The live stream: Home's hero, lower down, with who and what centred
 * beside the preview. */
constexpr float kPreviewY = 352.0f, kPreviewH = 450.0f;
const Rect kPreview{1024, kPreviewY, 800, kPreviewH};
constexpr float kInfoW = 680.0f;
constexpr float kWatchW = tw::kWatchW, kWatchH = tw::kWatchH;
constexpr float kTitleSize = 30.0f;
constexpr float kTitleLead = 42.0f;

/* The tabs: under the live stream, or where it would be while offline. Once
 * the page scrolls to them they sit this far from the top, which puts the
 * grid's first row where a category's is. */
constexpr float kLiveTabsTop = kPreviewY + kPreviewH + 58.0f;
constexpr float kOfflineTabsTop = 336.0f; /* clear of a description's third line */
constexpr float kTabsToGrid = tw::kTabH + tw::kTabsToChips + tw::kChipH + tw::kAboveGrid;
constexpr float kScrolledTabsY = 248.0f - kTabsToGrid;
const char *const kTabs[] = {"Videos", "Clips"};

/* The page keeps this much room over the row above the cursor (TwitchGrid's). */
constexpr float kTopRoom = 24.0f;
/* A tab's list that failed or came back empty: a panel where its cards go. */
constexpr float kNoteH = 200.0f;

/* A message instead of the page: Browse's spot for its own. */
constexpr float kMarkY = 352.0f;
const Rect kMessage{(kLeft + kRight) * 0.5f - 600.0f, 470, 1200, 160};

std::string str(const char *s)
{
    return std::string(s ? s : "");
}

/* The profile goes as the page leaves it, the stream a little later. */
float header_alpha(float page)
{
    return 1.0f - tween::clamp01(page / 160.0f);
}
float live_alpha(float page)
{
    return 1.0f - tween::clamp01(page / 640.0f);
}

/* Something on the page from top to bottom is worth drawing when some of it
 * is on screen. */
bool on_screen(float top, float bottom, float page)
{
    return top - page < ::hui::gfx::kVirtualHeight && bottom - page > 0.0f;
}

/* A video card's skeleton: the picture and its three lines. */
void draw_video_skeleton(DrawList &list, const Context &ctx, const Rect &r, float alpha)
{
    draw_card_skeleton(list, ctx, r, 0.82f, 0.56f, alpha);
    draw_skeleton(list, ctx, {r.x, r.y + r.h + tw::kTitleGap + 48, r.w * 0.38f, 20}, 8, alpha);
}

/* A small tag like LIVE, in other colours: OFFLINE on the followers' line. */
float draw_tag(DrawList &list, const Context &ctx, const char *text, float x, float y, float size, Color bg,
               Color ink)
{
    const ui::FontRef &font = ctx.fonts.semibold;
    const float tracking = size * 0.08f;
    const float pad = size * 0.55f;
    const float h = std::round(size * 1.6f);
    const float w = font.measure(text, size, tracking) + 2.0f * pad;
    list.rounded_rect({x, y, w, h}, size * 0.3f, bg);
    ui::text(list, font, text, x + w * 0.5f, y + h * 0.5f + size * 0.36f, size, ink, Align::center, tracking);
    return w;
}

/* Where the stream's lines and the Watch button go: one block, centred on
 * the preview's height. Gaps as on Home's hero; the tags, when there are
 * any, between the category and the button. */
struct InfoText
{
    float pill_y, title_y, game_y, tags_y;
    Rect watch;
};
InfoText info_text(int title_lines, bool has_game, bool has_tags = false)
{
    const float lines = static_cast<float>(std::max(title_lines, 1) - 1);
    const float pill_to_title = 29.0f + 58.0f; /* pill, then the title's baseline */
    const float title_to_last = kTitleLead * lines;
    /* No title the fonts can draw (all Japanese, say): the category takes
     * its line rather than leaving it blank. */
    const float last_to_game = has_game && title_lines > 0 ? 50.0f : 0.0f;
    const float game_to_tags = 22.0f;
    const float game_to_watch = has_tags ? game_to_tags + kTagH + 30.0f : 44.0f;
    const float height = pill_to_title + title_to_last + last_to_game + game_to_watch + kWatchH;
    InfoText t;
    t.pill_y = std::round(kPreview.y + (kPreview.h - height) * 0.5f);
    t.title_y = t.pill_y + pill_to_title;
    t.game_y = t.title_y + title_to_last + last_to_game;
    t.tags_y = t.game_y + game_to_tags;
    t.watch = {kLeft, t.game_y + game_to_watch, kWatchW, kWatchH};
    return t;
}

} // namespace

/* -------------------------------------------------------------------- state */

void TwitchChannelScreen::set(const evo_hui_twitch_channel_params_t &p)
{
    bool changed = false;
    auto take = [&](std::string &dst, const char *src) {
        src = src ? src : "";
        if (dst != src)
        {
            dst = src;
            changed = true;
        }
    };
    const std::string login = str(p.login);
    if (login != login_)
    {
        /* Another channel: its pictures fade in afresh. */
        avatar_motion_.art_in = 0.0f;
        preview_motion_.art_in = 0.0f;
    }
    take(login_, p.login);
    take(name_, p.name);
    take(description_, p.description);
    take(title_, p.title);
    take(game_, p.game);
    take(uptime_, p.uptime);
    const int tn = std::clamp(p.tag_count, 0, EVO_HUI_TWITCH_CHANNEL_TAGS);
    if (static_cast<int>(tags_.size()) != tn)
    {
        tags_.resize(static_cast<std::size_t>(tn));
        changed = true;
    }
    for (int i = 0; i < tn; ++i)
        take(tags_[static_cast<std::size_t>(i)], p.tags[i]);
    const int sn = std::clamp(p.schedule_count, 0, EVO_HUI_TWITCH_CHANNEL_SCHEDULE);
    if (static_cast<int>(schedule_.size()) != sn)
    {
        schedule_.resize(static_cast<std::size_t>(sn));
        changed = true;
    }
    for (int i = 0; i < sn; ++i)
    {
        take(schedule_[static_cast<std::size_t>(i)].when, p.schedule[i].when);
        take(schedule_[static_cast<std::size_t>(i)].title, p.schedule[i].title);
    }
    changed |= followers_ != p.followers || viewers_ != p.viewers || live_ != (p.live != 0);
    followers_ = p.followers;
    viewers_ = p.viewers;
    live_ = p.live != 0;
    if (changed)
        ++rev_;

    status_ = p.status;
    error_ = str(p.error);
    error_gone_ = p.error_gone != 0;
    avatar_ = twitch_picture(p.avatar);
    avatar_motion_.track(avatar_);
    preview_ = live_ ? twitch_picture(p.preview) : TwitchPicture{};
    preview_motion_.track(preview_);
    focus_ = p.focus;
    if (focus_ == EVO_HUI_TWITCH_FOCUS_WATCH && !live_)
        focus_ = EVO_HUI_TWITCH_FOCUS_TABS;
    rail_focused_ = p.rail_focused != 0;
    starting_ = p.starting != 0;

    tab_ = p.tab == EVO_HUI_TWITCH_CHANNEL_TAB_CLIPS ? EVO_HUI_TWITCH_CHANNEL_TAB_CLIPS
                                                     : EVO_HUI_TWITCH_CHANNEL_TAB_VIDEOS;
    filter_ = std::clamp(p.filter, 0, chip_count() - 1);
    filter_cursor_ = std::clamp(p.filter_cursor, 0, chip_count() - 1);
    cards_status_ = p.cards_status;
    cards_error_ = str(p.cards_error);
    const int n = std::clamp(p.card_count, 0, EVO_HUI_TWITCH_CHANNEL_CARDS);
    cards_.resize(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
    {
        TwitchVideo &v = cards_[static_cast<std::size_t>(i)];
        if (assign(v, p.cards[i]))
            card_motion_[i].art_in = 0.0f; /* another video in this slot */
        card_motion_[i].track(v.thumb);
    }
    cursor_ = std::clamp(p.cursor, 0, std::max(0, n - 1));
    if (focus_ == EVO_HUI_TWITCH_FOCUS_GRID && n == 0)
        focus_ = EVO_HUI_TWITCH_FOCUS_FILTERS;
    more_loading_ = p.more_loading != 0;
    starting_id_ = str(p.starting_id);
}

void TwitchChannelScreen::enter()
{
    age_ = 0.0f;
    ring_snapped_ = false;
    ring_alpha_.snap(0.0f);
    /* set() ran first: a page that is already here shows at once. */
    in_.snap(ready() ? 1.0f : 0.0f);
    watch_focus_.snap(!rail_focused_ && focus_ == EVO_HUI_TWITCH_FOCUS_WATCH ? 1.0f : 0.0f);
    page_.snap(page_target());
    cards_in_.snap(cards_.empty() ? 0.0f : 1.0f);
    ring_round_.snap(focus_ == EVO_HUI_TWITCH_FOCUS_GRID ? 0.0f : 1.0f);
    tab_line_.snap(static_cast<float>(tab_));
    tabs_focus_.snap(!rail_focused_ && ready() && focus_ == EVO_HUI_TWITCH_FOCUS_TABS ? 1.0f : 0.0f);
    shown_list_ = tab_ * 16 + filter_;
}

/* ------------------------------------------------------------------- layout */

float TwitchChannelScreen::tabs_top() const
{
    return live_ ? kLiveTabsTop : kOfflineTabsTop;
}

float TwitchChannelScreen::chips_top() const
{
    return tabs_top() + tw::kTabH + tw::kTabsToChips;
}

TwitchGrid TwitchChannelScreen::grid() const
{
    return TwitchGrid::make(kCols, 9.0f / 16.0f, tabs_top() + kTabsToGrid, tw::kVideoRowGap, tw::kVideoTextH);
}

int TwitchChannelScreen::chip_count() const
{
    return on_clips() ? EVO_HUI_TWITCH_CLIP_FILTERS : EVO_HUI_TWITCH_VIDEO_FILTERS;
}

float TwitchChannelScreen::chip_w() const
{
    return on_clips() ? kClipChipW : kVideoChipW;
}

Rect TwitchChannelScreen::watch_rect() const
{
    return info_text(static_cast<int>(lines_.title.size()), !lines_.game.empty(), !lines_.tags.empty()).watch;
}

float TwitchChannelScreen::page_target() const
{
    if (!ready() || focus_ == EVO_HUI_TWITCH_FOCUS_WATCH || focus_ == EVO_HUI_TWITCH_FOCUS_NONE)
        return 0.0f;
    const TwitchGrid g = grid();
    /* Not at all while the row fits; then with the tabs at the top. */
    const float tabs = std::max(0.0f, tabs_top() - kScrolledTabsY);
    auto bottom = [&](int row) { return g.top + static_cast<float>(row) * g.pitch() + g.card_h + g.text_h; };
    auto fits = [&](int row, float page) { return bottom(row) - page <= kHintBandTop; };
    const int row = focus_ == EVO_HUI_TWITCH_FOCUS_GRID && !cards_.empty() ? cursor_ / kCols : 0;
    if (fits(row, 0.0f))
        return 0.0f;
    if (fits(row, tabs) || row == 0)
        return tabs;
    /* Deeper: the row above the cursor's at the top, in full. */
    return std::max(tabs, g.top + static_cast<float>(row - 1) * g.pitch() - kTopRoom);
}

Rect TwitchChannelScreen::ring_target() const
{
    if (rail_focused_ || !ready())
        return {};
    const float page = page_target();
    Rect r;
    switch (focus_)
    {
    case EVO_HUI_TWITCH_FOCUS_WATCH:
        return live_ ? watch_rect() : Rect{}; /* the ring waits there, unseen, for a move to the tabs */
    case EVO_HUI_TWITCH_FOCUS_TABS:
        return {}; /* their row rings the open one itself (draw_tab_row) */
    case EVO_HUI_TWITCH_FOCUS_FILTERS:
        r = chip_rect(filter_cursor_, chips_top(), chip_w()).inset(-7.0f);
        break;
    case EVO_HUI_TWITCH_FOCUS_GRID:
        if (cards_.empty())
            return {};
        r = focus_ring_rect(grid().cell(cursor_));
        break;
    default:
        return {};
    }
    r.y -= page;
    return r;
}

void TwitchChannelScreen::update(float dt)
{
    age_ += dt;
    in_.target = ready() ? 1.0f : 0.0f;
    in_.update(dt, 9.0f);
    page_.target = page_target();
    page_.update(dt, 10.0f);

    /* Another tab or chip, or a list emptied for a refresh: no cards to fade
     * out, so the skeletons show at once. */
    const int list = tab_ * 16 + filter_;
    if (list != shown_list_ || cards_.empty())
        cards_in_.snap(cards_.empty() ? 0.0f : 1.0f);
    shown_list_ = list;
    cards_in_.target = cards_.empty() ? 0.0f : 1.0f;
    cards_in_.update(dt, 9.0f);
    tab_line_.target = static_cast<float>(tab_);
    tab_line_.update(dt, 18.0f);
    tabs_focus_.target = !rail_focused_ && ready() && focus_ == EVO_HUI_TWITCH_FOCUS_TABS ? 1.0f : 0.0f;
    tabs_focus_.update(dt, 16.0f);

    const Rect target = ring_target();
    const bool lit = target.w > 0.0f && focus_ != EVO_HUI_TWITCH_FOCUS_WATCH;
    const float round = focus_ == EVO_HUI_TWITCH_FOCUS_GRID ? 0.0f : 1.0f;
    if (!ring_snapped_ && target.w > 0.0f)
    {
        /* The first place the ring has: no flight in from the corner. */
        ring_.snap(target);
        ring_round_.snap(round);
        page_.snap(page_.target);
        ring_snapped_ = true;
    }
    if (target.w > 0.0f)
        ring_.target(target);
    ring_.update(dt, 20.0f);
    ring_round_.target = round;
    ring_round_.update(dt, 20.0f);
    ring_alpha_.target = lit ? 1.0f : 0.0f;
    ring_alpha_.update(dt, 14.0f);
    watch_focus_.target =
        !rail_focused_ && ready() && live_ && focus_ == EVO_HUI_TWITCH_FOCUS_WATCH ? 1.0f : 0.0f;
    watch_focus_.update(dt, 16.0f);
    avatar_motion_.step(dt, false, false);
    preview_motion_.step(dt, false, false);
    const bool in_grid = !rail_focused_ && ready() && focus_ == EVO_HUI_TWITCH_FOCUS_GRID && !cards_.empty();
    for (int i = 0; i < EVO_HUI_TWITCH_CHANNEL_CARDS; ++i)
        card_motion_[i].step(dt, in_grid, in_grid && i == cursor_);
}

/* ------------------------------------------------------------------ profile */

void TwitchChannelScreen::draw_header(DrawList &list, const Context &ctx, float alpha) const
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    Lines &l = lines_;
    if (l.fonts != &ctx.fonts || l.rev != rev_)
    {
        l.fonts = &ctx.fonts;
        l.rev = rev_;
        const std::string name = drawable(fonts.display, name_);
        l.name = fonts.display.font->fit(name.empty() ? login_ : name, 56, kRight - kNameX);
        l.followers = followers_ < 0 ? std::string()
                                     : format_viewers(followers_) + (followers_ == 1 ? " follower" : " followers");
        l.description = fit_lines(fonts.regular, drawable(fonts.regular, description_), kDescSize,
                                  schedule_.empty() ? kDescW : kDescWSched, 3);
        l.title = fit_lines(fonts.regular, drawable(fonts.regular, title_), kTitleSize, kInfoW, 2);
        l.game = fonts.semibold.font->fit(drawable(fonts.semibold, game_), 26, kInfoW);
        l.watching = format_viewers(viewers_) + " watching";
        if (!uptime_.empty())
            l.watching += " \xC2\xB7 " + uptime_;
        /* As many tags as fit on the line, each cut to a pill's width. */
        l.tags.clear();
        l.tag_w.clear();
        float x = 0.0f;
        for (const std::string &tag : tags_)
        {
            const std::string text = fonts.semibold.font->fit(drawable(fonts.semibold, tag), kTagText, kTagMaxW);
            if (text.empty())
                continue;
            const float w = fonts.semibold.measure(text, kTagText) + 2.0f * kTagPad;
            if (x + w > kInfoW)
                break;
            l.tags.push_back(text);
            l.tag_w.push_back(w);
            x += w + kTagGap;
        }
        l.schedule.clear();
        for (const Scheduled &s : schedule_)
            l.schedule.push_back(fonts.regular.font->fit(drawable(fonts.regular, s.title), kSchedText,
                                                         kSchedW - 2.0f * kSchedPad - kSchedWhenW));
    }
    if (alpha <= 0.01f)
        return;

    list.push_opacity(alpha);
    const float round = kAvatar.w * 0.5f;
    list.shadow({kAvatar.x, kAvatar.y + 10, kAvatar.w, kAvatar.h}, round, 22, Color::rgb(0x000000, 0.45f));
    const std::uint32_t tex = twitch_texture(ctx, avatar_);
    if (avatar_motion_.art_in < 1.0f)
        draw_skeleton(list, ctx, kAvatar, round);
    if (tex)
        list.image(tex, kAvatar, cover_uv(avatar_.w, avatar_.h, kAvatar.w, kAvatar.h),
                   kWhite.with_alpha(avatar_motion_.art_in), round);
    list.bordered_rect(kAvatar, round, kClear, 1.5f, kWhite.with_alpha(0.08f));

    ui::text(list, fonts.display, l.name, kNameX - 2, kTitleY, 56, p.text);
    /* Offline: the tag where the stream's LIVE would be, before the followers. */
    float x = kNameX;
    if (!live_)
        x += draw_tag(list, ctx, "OFFLINE", kNameX, kLineY - 22, 16, kWhite.with_alpha(0.14f), p.text_muted) + 14;
    if (!l.followers.empty())
        ui::text(list, fonts.regular, l.followers, x, kLineY, 26, p.text_muted);
    for (std::size_t i = 0; i < l.description.size(); ++i)
        ui::text(list, fonts.regular, l.description[i], kNameX, kDescY + kDescLead * static_cast<float>(i), kDescSize,
                 p.text.with_alpha(0.78f));
    list.pop_opacity();
    draw_schedule(list, ctx, alpha);
}

void TwitchChannelScreen::draw_schedule(DrawList &list, const Context &ctx, float alpha) const
{
    const int n = static_cast<int>(std::min(schedule_.size(), lines_.schedule.size()));
    if (n == 0 || alpha <= 0.01f)
        return;
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    list.push_opacity(alpha);
    const float h = kSchedRowY - kSchedTop + kSchedLead * static_cast<float>(n - 1) + 18.0f;
    const Rect panel{kSchedX, kSchedTop, kSchedW, h};
    list.rounded_rect(panel, 18, kWhite.with_alpha(0.05f));
    list.bordered_rect(panel, 18, kClear, 1.5f, kWhite.with_alpha(0.08f));
    ui::text(list, fonts.semibold, "SCHEDULE", panel.x + kSchedPad, kSchedEyebrowY, 16, p.accent_alt, Align::left,
             2.0f);
    for (int i = 0; i < n; ++i)
    {
        const float y = kSchedRowY + kSchedLead * static_cast<float>(i);
        ui::text(list, fonts.semibold, schedule_[static_cast<std::size_t>(i)].when, panel.x + kSchedPad, y, kSchedText,
                 p.text);
        ui::text(list, fonts.regular, lines_.schedule[static_cast<std::size_t>(i)], panel.x + kSchedPad + kSchedWhenW,
                 y, kSchedText, p.text_muted);
    }
    list.pop_opacity();
}

/* --------------------------------------------------------------------- live */

void TwitchChannelScreen::draw_live(DrawList &list, const Context &ctx, float alpha, std::uint32_t tex) const
{
    if (alpha <= 0.01f)
        return;
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    const Lines &l = lines_;
    list.push_opacity(alpha);

    /* The preview, as Home's hero draws its picture. */
    const Rect art = kPreview;
    list.shadow({art.x, art.y + 28, art.w, art.h}, 24, 48, Color::rgb(0x000000, 0.55f));
    if (preview_motion_.art_in < 1.0f)
        draw_skeleton(list, ctx, art, 16);
    if (tex)
        list.image(tex, art, cover_uv(preview_.w, preview_.h, art.w, art.h), kWhite.with_alpha(preview_motion_.art_in),
                   16);
    list.gradient_rect({art.x, art.y + art.h * 0.6f, art.w, art.h * 0.4f}, 16, kClear, Color::rgb(0x000000, 0.32f));
    list.bordered_rect(art, 16, kClear, 1.5f, kWhite.with_alpha(0.08f));

    /* What is on: LIVE, the viewers and how long, the title, the category,
     * the tags. */
    const InfoText t = info_text(static_cast<int>(l.title.size()), !l.game.empty(), !l.tags.empty());
    const float pill_w = draw_live_pill(list, ctx, kLeft, t.pill_y, 18);
    ui::text(list, fonts.semibold, l.watching, kLeft + pill_w + 16, t.pill_y + 23, 24, p.text_muted);
    for (std::size_t i = 0; i < l.title.size(); ++i)
        ui::text(list, fonts.regular, l.title[i], kLeft, t.title_y + kTitleLead * static_cast<float>(i), kTitleSize,
                 p.text.with_alpha(0.86f));
    if (!l.game.empty())
        ui::text(list, fonts.semibold, l.game, kLeft, t.game_y, 26, p.accent_alt);
    float tx = kLeft;
    for (std::size_t i = 0; i < l.tags.size(); ++i)
    {
        const Rect chip{tx, t.tags_y, l.tag_w[i], kTagH};
        list.rounded_rect(chip, kTagH * 0.5f, kWhite.with_alpha(0.08f));
        ui::text(list, fonts.semibold, l.tags[i], chip.cx(), chip.cy() + kTagText * 0.36f, kTagText,
                 p.text.with_alpha(0.72f), Align::center);
        tx += l.tag_w[i] + kTagGap;
    }

    /* Watch live: white with dark ink when it has the cursor, as on Home. */
    draw_watch_button(list, ctx, t.watch, watch_focus_.value, starting_);
    list.pop_opacity();
}

/* ------------------------------------------------------------ tabs and cards */

void TwitchChannelScreen::draw_tabs(DrawList &list, const Context &ctx, float alpha) const
{
    draw_tab_row(list, ctx, kTabs, 2, tab_, tab_line_.value, tabs_top(), alpha, tabs_focus_.value);
    draw_chip_row(list, ctx, on_clips() ? kClipFilterLabels : kVideoFilterLabels, chip_count(), filter_, chips_top(),
                  chip_w(), alpha);
}

void TwitchChannelScreen::draw_cards(DrawList &list, const Context &ctx, float alpha, float page) const
{
    if (alpha <= 0.01f)
        return;
    const TwitchGrid g = grid();
    const int n = static_cast<int>(cards_.size());
    if (n == 0 && cards_status_ != EVO_HUI_TWITCH_LOADING)
    {
        const Rect note{kLeft, g.top, kView, kNoteH};
        if (cards_status_ == EVO_HUI_TWITCH_ERROR)
            draw_twitch_message(list, ctx, note, cards_error_, "to try again", alpha);
        else
            draw_twitch_message(list, ctx, note, on_clips() ? empty_clips_text(filter_) : empty_videos_text(filter_),
                                "to refresh", alpha);
        return;
    }

    list.push_opacity(alpha);
    const float in = cards_in_.value;
    if (in < 0.99f)
    {
        /* Two rows of skeletons: how many there are isn't known yet. */
        for (int i = 0; i < kCols * 2; ++i)
        {
            const Rect r = g.cell(i);
            if (on_screen(r.y, r.y + r.h + g.text_h, page))
                draw_video_skeleton(list, ctx, r, 1.0f - in);
        }
    }
    if (in > 0.01f)
    {
        list.push_opacity(in);
        const int focused = rail_focused_ || focus_ != EVO_HUI_TWITCH_FOCUS_GRID ? -1 : cursor_;
        auto draw_one = [&](int i) {
            const TwitchVideo &v = cards_[static_cast<std::size_t>(i)];
            draw_video_card(list, ctx, v, card_motion_[i], g.cell(i), i == focused,
                            !starting_id_.empty() && starting_id_ == v.id);
        };
        for (int i = 0; i < n; ++i)
        {
            const Rect r = g.cell(i);
            if (i != focused && on_screen(r.y, r.y + r.h + g.text_h, page))
                draw_one(i);
        }
        if (focused >= 0 && focused < n)
            draw_one(focused);
        /* The next page on its way: skeletons where its first cards go. */
        if (more_loading_ && n > 0)
        {
            const int extra = n % kCols == 0 ? kCols : kCols - n % kCols;
            for (int i = n; i < n + extra; ++i)
            {
                const Rect r = g.cell(i);
                if (on_screen(r.y, r.y + r.h + g.text_h, page))
                    draw_video_skeleton(list, ctx, r, 1.0f);
            }
        }
        list.pop_opacity();
    }
    list.pop_opacity();
}

/* ------------------------------------------------------------------ loading */

void TwitchChannelScreen::draw_loading(DrawList &list, const Context &ctx, float alpha) const
{
    if (alpha <= 0.01f)
        return;
    draw_skeleton(list, ctx, kAvatar, kAvatar.w * 0.5f, alpha);
    draw_skeleton(list, ctx, {kNameX, kTitleY - 44, 420, 48}, 10, alpha);
    draw_skeleton(list, ctx, {kNameX, kLineY - 22, 200, 26}, 8, alpha);
    draw_skeleton(list, ctx, {kNameX, kDescY - 20, 760, 24}, 8, alpha);
    draw_skeleton(list, ctx, {kNameX, kDescY + kDescLead - 20, 540, 24}, 8, alpha);

    const InfoText t = info_text(2, true);
    draw_skeleton(list, ctx, kPreview, 16, alpha);
    draw_skeleton(list, ctx, {kLeft, t.pill_y, 72, 29}, 6, alpha);
    draw_skeleton(list, ctx, {kLeft + 88, t.pill_y + 4, 180, 22}, 8, alpha);
    draw_skeleton(list, ctx, {kLeft, t.title_y - 24, 620, 28}, 8, alpha);
    draw_skeleton(list, ctx, {kLeft, t.title_y + kTitleLead - 24, 460, 28}, 8, alpha);
    draw_skeleton(list, ctx, {kLeft, t.game_y - 22, 220, 26}, 8, alpha);
    draw_skeleton(list, ctx, t.watch, 32, alpha);
}

/* --------------------------------------------------------------------- draw */

void TwitchChannelScreen::draw(DrawList &list, const Context &ctx) const
{
    /* A live channel's preview washes in behind everything, as Home's hero;
     * an offline one's avatar, fainter, so the page still has its colour. */
    const std::uint32_t preview_tex = ready() && live_ ? twitch_texture(ctx, preview_) : 0;
    if (preview_tex)
        draw_backdrop(list, ctx, preview_tex, 0.45f * preview_motion_.art_in);
    else if (ready() && !live_)
        draw_backdrop(list, ctx, twitch_texture(ctx, avatar_), 0.3f * avatar_motion_.art_in);
    else
        draw_backdrop(list, ctx);

    const float in = tween::stagger(age_, 0, 0.06f, 0.5f);
    const float slide = 16.0f * (1.0f - in);
    const float page = page_.value;
    list.push_clip({NavRail::kWidth, 0, ::hui::gfx::kVirtualWidth - NavRail::kWidth, ::hui::gfx::kVirtualHeight});
    if (status_ == EVO_HUI_TWITCH_ERROR)
    {
        const float a = tween::stagger(age_, 1, 0.08f, 0.6f);
        draw_message_mark(list, ctx, kMarkY, a);
        if (error_gone_)
            draw_twitch_message(list, ctx, kMessage, error_, "to go back", a, false, ui::Button::circle);
        else
            draw_twitch_message(list, ctx, kMessage, error_, "to try again", a, false);
    }
    else
    {
        const float ready_in = in_.value;
        list.push_transform(1.0f, 0, 0, 0, slide - page);
        if (ready_in < 0.99f)
            draw_loading(list, ctx, in * (1.0f - ready_in));
        if (ready_in > 0.01f)
        {
            /* The ring: a pill on a tab or a chip, a card's corners on a
             * card; its light only under a card. Both in screen space. */
            const Rect ring = ring_.value();
            const float round = ring_round_.value;
            const float radius = tween::lerp(kFocusRingRadius, ring.h * 0.5f, round);
            const float tabs = tween::stagger(age_, 2, 0.08f, 0.6f) * ready_in;
            list.push_transform(1.0f, 0, 0, 0, page - slide);
            draw_focus_glow(list, ctx, ring, ring_alpha_.value * tabs * (1.0f - round), radius);
            list.pop_transform();

            draw_header(list, ctx, in * ready_in * header_alpha(page));
            const float body = tween::stagger(age_, 1, 0.08f, 0.6f) * ready_in;
            if (live_)
            {
                list.push_transform(1.0f, 0, 0, 30.0f * (1.0f - body), 0);
                draw_live(list, ctx, body * live_alpha(page), preview_tex);
                list.pop_transform();
            }
            draw_tabs(list, ctx, tabs);
            draw_cards(list, ctx, tween::stagger(age_, 3, 0.08f, 0.6f) * ready_in, page);

            list.push_transform(1.0f, 0, 0, 0, page - slide);
            draw_focus_ring(list, ctx, ring, ring_alpha_.value * tabs, radius);
            list.pop_transform();
        }
        list.pop_transform();
    }
    list.pop_clip();
    if (ready())
        draw_hint_band(list, ctx);

    ui::Hint hints[3];
    int h = 0;
    if (rail_focused_)
        h = rail_hints(hints);
    else
    {
        if (ready() && live_ && focus_ == EVO_HUI_TWITCH_FOCUS_WATCH)
            hints[h++] = {ui::Button::cross, "Watch"};
        else if (ready() && focus_ == EVO_HUI_TWITCH_FOCUS_GRID && !cards_.empty())
            hints[h++] = {ui::Button::cross, "Watch"};
        else if (ready() && focus_ == EVO_HUI_TWITCH_FOCUS_FILTERS && filter_cursor_ != filter_)
            hints[h++] = {ui::Button::cross, "Select"};
        if (!(status_ == EVO_HUI_TWITCH_ERROR && error_gone_))
            hints[h++] = {ui::Button::triangle, "Refresh"};
        hints[h++] = {ui::Button::circle, "Back"};
    }
    draw_twitch_hints(list, ctx, hints, h, tween::stagger(age_, 6, 0.08f, 0.5f));
}

/* -------------------------------------------------------------------- focus */

bool TwitchChannelScreen::focus(FocusInfo *out) const
{
    out->focusable.clear();
    if (!ready() || rail_focused_)
        return false;
    const char *const *labels = on_clips() ? kClipFilterLabels : kVideoFilterLabels;
    const std::string kind = on_clips() ? "clip-" : "video-";
    if (live_)
        out->focusable.emplace_back("watch", "Watch " + name_);
    out->focusable.emplace_back("tab-videos", kTabs[0]);
    out->focusable.emplace_back("tab-clips", kTabs[1]);
    for (int i = 0; i < chip_count(); ++i)
        out->focusable.emplace_back("filter-" + std::to_string(i), labels[i]);
    for (std::size_t i = 0; i < cards_.size(); ++i)
        out->focusable.emplace_back(kind + std::to_string(i), cards_[i].title);

    const float page = page_target();
    switch (focus_)
    {
    case EVO_HUI_TWITCH_FOCUS_WATCH:
        if (!live_)
            break;
        out->id = "watch";
        out->text = "Watch " + name_;
        out->index = out->total = 1;
        out->rect = watch_rect();
        return true;
    case EVO_HUI_TWITCH_FOCUS_TABS:
        out->id = on_clips() ? "tab-clips" : "tab-videos";
        out->text = kTabs[tab_];
        out->index = tab_ + 1;
        out->total = 2;
        out->rect = tab_rect(tab_, tabs_top());
        out->rect.y -= page;
        return true;
    case EVO_HUI_TWITCH_FOCUS_FILTERS:
        out->id = "filter-" + std::to_string(filter_cursor_);
        out->text = std::string(labels[filter_cursor_]) + (filter_cursor_ == filter_ ? ", selected" : "");
        out->index = filter_cursor_ + 1;
        out->total = chip_count();
        out->rect = chip_rect(filter_cursor_, chips_top(), chip_w());
        out->rect.y -= page;
        return true;
    case EVO_HUI_TWITCH_FOCUS_GRID:
        if (cards_.empty())
            break;
        out->id = kind + std::to_string(cursor_);
        out->text = cards_[static_cast<std::size_t>(cursor_)].title;
        out->index = cursor_ + 1;
        out->total = static_cast<int>(cards_.size());
        out->rect = grid().cell(cursor_);
        out->rect.y -= page;
        return true;
    default:
        break;
    }
    out->focusable.clear();
    return false;
}

} // namespace evo::kit
