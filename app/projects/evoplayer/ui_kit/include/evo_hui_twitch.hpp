/*
 * evo_hui_twitch.hpp - the Twitch screens on ps5-homebrew-ui, and the pieces
 * they share (layout, cards, skeletons, the focus ring, viewer counts).
 *
 * Same contract as evo_hui_screens.hpp: core/ owns focus and data and hands
 * over a params struct (evo_hui_twitch.h); these classes only animate towards
 * it and draw. Coordinates are the kit's 1920 x 1080 virtual canvas.
 */
#ifndef EVO_HUI_TWITCH_HPP
#define EVO_HUI_TWITCH_HPP

#include "evo_hui_screens.hpp"
#include "evo_hui_twitch.h"

#include "core/tween.hpp"
#include "gfx/draw_list.hpp"
#include "ui/glyphs.hpp"
#include "ui/motion.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace evo::kit
{

/* Where every Twitch screen puts things. */
namespace tw
{
constexpr float kLeft = NavRail::kWidth + 64.0f; /* content's left edge */
constexpr float kRight = 1824.0f;                /* and its right edge */
constexpr float kView = kRight - kLeft;
constexpr float kGap = 24.0f;        /* between cards */
constexpr float kRowGap = 104.0f;    /* card bottom to the next card in a grid (its two lines fit) */
constexpr float kRadius = 12.0f;     /* card corners */
constexpr float kFocusScale = 1.05f; /* the focused card grows this much */
/* Card bottom to the title's baseline: clear of the focus ring, which comes
 * 2.5 % of the card's height plus 7 px below it (15 px on Browse's cards). */
constexpr float kTitleGap = 42.0f;
constexpr float kSubGap = 32.0f;     /* title baseline to the line under it */
constexpr float kTextH = 84.0f;      /* the two lines under a card, descenders included */
/* A category card's lines (its name can take two), and the row gap that
 * keeps them clear of a focused card's ring in the row below. */
constexpr float kCatTextH = 116.0f;
constexpr float kCatRowGap = 144.0f;
/* "Watch live", on Home's hero and on a channel page. */
constexpr float kWatchW = 300.0f;
constexpr float kWatchH = 64.0f;
/* Tabs (a channel's Videos and Clips, a category's Live and Clips) and the
 * filter chips under them. */
constexpr float kTabW = 168.0f;
constexpr float kTabH = 52.0f;
constexpr float kTabGap = 16.0f;
constexpr float kChipH = 44.0f;
constexpr float kChipGap = 12.0f;
constexpr float kTabsToChips = 24.0f; /* the tabs' foot to the chips' top */
constexpr float kAboveGrid = 36.0f;   /* the tabs' or the chips' foot to the cards' top */
/* Video and clip cards have three lines under them (the title, views and age,
 * the detail): a category card's room. */
constexpr float kVideoTextH = kCatTextH;
constexpr float kVideoRowGap = kCatRowGap;
} // namespace tw

/* The app's mark: the "twi" tile with rounded corners, `size` square, (x, y)
 * its top left. The pictures are assets/icons/icon_app.png and
 * icon_app_small.png (tools/gen_brand.py); until one is up it draws the plain
 * purple tile. */
void draw_app_mark(hui::gfx::DrawList &list, const Context &ctx, float x, float y, float size);

/* The mark's "twi" lettering alone, in one colour, for the quiet places (an
 * empty or failed page): `width` wide and half as tall, (x, y) the top left of
 * that box. Draws nothing until assets/icons/icon_app_mono.png is up. */
void draw_app_lettering(hui::gfx::DrawList &list, const Context &ctx, float x, float y, float width,
                        hui::gfx::Color color);

/* A placeholder block: a rounded surface with a light band sweeping across it.
 * The band runs on Context::time, so all blocks on screen light up left to
 * right as one surface. */
void draw_skeleton(hui::gfx::DrawList &list, const Context &ctx, const hui::gfx::Rect &r, float radius,
                   float alpha = 1.0f);

/* A card's skeleton with its two lines under it; the lines are title_w and
 * sub_w of the card's width. */
void draw_card_skeleton(hui::gfx::DrawList &list, const Context &ctx, const hui::gfx::Rect &card, float title_w,
                        float sub_w, float alpha);

/* Twitch's live red. */
inline hui::gfx::Color twitch_live_red() { return hui::gfx::Color::rgb(0xeb0400); }

/* A viewer count the short way: 999, 1.2K, 12.4K, 120K, 1.1M. */
std::string format_viewers(int viewers);

/* The part of a w x h picture that fills a box of the given size (centred). */
hui::gfx::Rect cover_uv(int w, int h, float box_w, float box_h);

/* r grown about its centre. */
hui::gfx::Rect scaled(const hui::gfx::Rect &r, float s);

/* The red "LIVE" tag with its top left at (x, y); `size` is the text size.
 * Returns its width. */
float draw_live_pill(hui::gfx::DrawList &list, const Context &ctx, float x, float y, float size);

/* A turning arc: something is on its way. */
void draw_spinner(hui::gfx::DrawList &list, const Context &ctx, float cx, float cy, float radius,
                  hui::gfx::Color color);

/* ------------------------------------------------------------------- cards */

/* A picture as a screen keeps it from set() to draw(): the image service's
 * pixels, read as EVO's 0xAABBGGRR words. px is null until it has arrived. */
struct TwitchPicture
{
    const std::uint32_t *px = nullptr;
    int w = 0, h = 0;
    std::uint64_t id = 0;
};
TwitchPicture twitch_picture(const evo_hui_twitch_image_t &in);
/* Its texture this frame; 0 while it has not arrived. Keyed by the picture's
 * id (TextureCache::pixels_immutable): the service never changes a buffer
 * under the same id. */
std::uint32_t twitch_texture(const Context &ctx, const TwitchPicture &pic);

/* text without the characters `font` has no glyph for (emoji, and scripts
 * the baked fonts leave out), which would otherwise draw as '?'. A run of
 * them becomes one space, none next to a space and none before punctuation
 * (which also loses a space in front of the run): "GG🔥WP" reads "GG WP",
 * "Hype 🎉!" reads "Hype!". Control characters become spaces; spaces
 * collapse and the ends are trimmed. */
std::string drawable(const hui::ui::FontRef &font, const std::string &text);

/* text broken into at most max_lines lines no wider than width; the last
 * line ends in an ellipsis when the rest does not fit. */
std::vector<std::string> fit_lines(const hui::ui::FontRef &font, const std::string &text, float size, float width,
                                   int max_lines);

/* drawable() text cut to a width with Font::fit (which measures every
 * prefix), kept until the text, the font, the size or the width changes.
 * draw() is const, so the cache is mutable. Main thread only. */
class FittedText
{
  public:
    const std::string &get(const hui::ui::FontRef &font, const std::string &text, float size, float width) const;

  private:
    mutable const hui::gfx::Font *font_ = nullptr;
    mutable float size_ = -1.0f, width_ = -1.0f;
    mutable std::string text_, out_;
};

/* The lines a card draws, worked out again only when the card's text (rev),
 * the fonts or the width change. */
struct CardLines
{
    const void *fonts = nullptr;
    float width = -1.0f;
    std::uint32_t rev = ~0u;
    std::string title, title2, sub; /* cut to the width; title2 for a category's second line */
    std::string chip;       /* a stream's viewer count */
    float chip_w = 0.0f;
};

struct TwitchStream
{
    std::string login, name, title, game;
    int viewers = 0;
    TwitchPicture thumb;
    std::uint32_t rev = 0; /* goes up whenever the text or the count changes */
    mutable CardLines lines;
};

struct TwitchCategory
{
    std::string name;
    int viewers = -1;
    TwitchPicture art;
    std::uint32_t rev = 0;
    mutable CardLines lines;
};

/* A channel from a search: avatar, name, live or not. */
struct TwitchChannel
{
    std::string login, name;
    int viewers = -1; /* -1 offline */
    TwitchPicture avatar;
    std::uint32_t rev = 0;
    mutable CardLines lines; /* title: the name; sub: "N watching" or "Offline" */
};

/* A past broadcast, highlight, upload or clip (evo_hui_twitch_video_t). */
struct TwitchVideo
{
    std::string id, title, detail, age, chip; /* chip: "" for the length */
    int views = -1, seconds = 0;
    float progress = -1.0f;
    TwitchPicture thumb;
    std::uint32_t rev = 0;
    mutable CardLines lines; /* title; sub: "N views · age"; title2: the detail; chip: the length */
};

/* Copy a params entry in. True when the slot now holds another channel or
 * category than before (its picture then fades in afresh). */
bool assign(TwitchStream &out, const evo_hui_twitch_stream_t &in);
bool assign(TwitchVideo &out, const evo_hui_twitch_video_t &in);
bool assign(TwitchCategory &out, const evo_hui_twitch_category_t &in);
bool assign(TwitchChannel &out, const evo_hui_twitch_channel_t &in);

/* Per card: how focused it is, how far it steps back while a neighbour has the
 * cursor, and how far its picture has faded in. */
struct CardMotion
{
    hui::tween::Spring focus;
    hui::tween::Spring dim;
    float art_in = 0.0f;
    std::uint64_t art_id = 0;

    /* After set(): a picture that has just arrived fades in; a newer picture
     * of the same card (a refresh) replaces it without a fade. */
    void track(const TwitchPicture &pic);
    /* group_focused: the cursor is on this card's row or grid. */
    void step(float dt, bool group_focused, bool focused);
    /* The picture's tint: the cards beside the cursor step back a little. */
    hui::gfx::Color tint() const;
};

/* A 16:9 stream card: thumbnail with the LIVE tag and the viewer count, the
 * title and "channel · category" under it. While `starting` the thumbnail
 * dims under a spinner. The card grows with m.focus. */
void draw_stream_card(hui::gfx::DrawList &list, const Context &ctx, const TwitchStream &s, const CardMotion &m,
                      const hui::gfx::Rect &r, bool focused, bool starting);

/* A 3:4 category card: box art, the name (two lines at most, see
 * tw::kCatTextH) and "N watching" under it. */
void draw_category_card(hui::gfx::DrawList &list, const Context &ctx, const TwitchCategory &c, const CardMotion &m,
                        const hui::gfx::Rect &r, bool focused);

/* A video or clip card: the thumbnail with its length at the top left and,
 * when some of it was watched, a bar along its foot; then the title,
 * "N views · 3 days ago" and the detail. While `starting` the thumbnail dims
 * under a spinner. The card grows with m.focus. */
void draw_video_card(hui::gfx::DrawList &list, const Context &ctx, const TwitchVideo &v, const CardMotion &m,
                     const hui::gfx::Rect &r, bool focused, bool starting);

/* A length the short way: "1:02:05", "5:30", "0:30". */
std::string format_duration(int seconds);

/* The labels of a Videos tab's chips (EVO_HUI_TWITCH_VIDEOS_*) and a Clips
 * tab's (EVO_HUI_TWITCH_CLIPS_*), and the width every chip of that row has. */
extern const char *const kVideoFilterLabels[EVO_HUI_TWITCH_VIDEO_FILTERS];
extern const char *const kClipFilterLabels[EVO_HUI_TWITCH_CLIP_FILTERS];
constexpr float kVideoChipW = 236.0f;
constexpr float kClipChipW = 196.0f;

/* A row of tabs with its top at y, the labels one after another from
 * tw::kLeft: the open one in full ink over an accent line (`line` is where
 * that line is, as a tab index, so it can slide), the others muted. While the
 * row has the cursor (`focus` going to 1) a ring hugs the open tab's label;
 * the screens' own ring stays off the tabs, whose widths only the fonts know.
 * tab_rect() is roughly where a tab is, for the dev remote's focus report. */
hui::gfx::Rect tab_rect(int index, float y);
void draw_tab_row(hui::gfx::DrawList &list, const Context &ctx, const char *const *labels, int count, int open,
                  float line, float y, float alpha, float focus);

/* A row of filter chips from tw::kLeft with its top at y, each chip_w wide:
 * the selected one filled white with dark ink, the others quiet. */
hui::gfx::Rect chip_rect(int index, float y, float chip_w);
void draw_chip_row(hui::gfx::DrawList &list, const Context &ctx, const char *const *labels, int count,
                   int selected, float y, float chip_w, float alpha);

/* "Nothing in the last 7 days"-style sentences for a list that came back
 * empty: a channel's Videos tab (EVO_HUI_TWITCH_VIDEOS_*), and a Clips tab
 * (EVO_HUI_TWITCH_CLIPS_*). */
const char *empty_videos_text(int filter);
const char *empty_clips_text(int filter);

/* The focus ring around a card at rest: the card grown by kFocusScale, plus room. */
hui::gfx::Rect focus_ring_rect(const hui::gfx::Rect &card);
/* The ring's corner radius around a card (radius < 0 in the two below). */
constexpr float kFocusRingRadius = tw::kRadius * tw::kFocusScale + 7.0f;
/* The ring's light, drawn before the cards (a glow fills its inside). radius:
 * the ring's corners, the card's when < 0 (half its width rings a circle). */
void draw_focus_glow(hui::gfx::DrawList &list, const Context &ctx, const hui::gfx::Rect &ring, float alpha,
                     float radius = -1.0f);
/* The ring itself, drawn after the focused card. */
void draw_focus_ring(hui::gfx::DrawList &list, const Context &ctx, const hui::gfx::Rect &ring, float alpha,
                     float radius = -1.0f);

/* One sentence in the middle of `area`, and "Press [button] <hint>" under
 * it ("to try again"); on a quiet panel when `panel`. An empty text reads
 * "Something went wrong". */
void draw_twitch_message(hui::gfx::DrawList &list, const Context &ctx, const hui::gfx::Rect &area,
                         const std::string &text, const char *hint, float alpha, bool panel = true,
                         hui::ui::Button button = hui::ui::Button::triangle);

/* "Press [button] <hint>" centred on cx, on baseline `base`. */
void draw_press_hint(hui::gfx::DrawList &list, const Context &ctx, float cx, float base, hui::ui::Button button,
                     const char *hint);

/* The faint mark that sits over a page-wide message (draw_twitch_message
 * with no panel), centred on the content at `y`: the page is not broken,
 * just empty. */
void draw_message_mark(hui::gfx::DrawList &list, const Context &ctx, float y, float alpha);

/* A quiet line beside a row's title (`title`, semibold 30 from tw::kLeft, on
 * baseline y) when the row has nothing to show: the sentence, and with a
 * hint ". Press [triangle] <hint>" after it. */
void draw_row_note(hui::gfx::DrawList &list, const Context &ctx, const char *title, float y,
                   const std::string &sentence, const char *hint);

/* "<i> of <n>" right-aligned on a row's title line, for the row the cursor is in. */
void draw_row_counter(hui::gfx::DrawList &list, const Context &ctx, float y, int index, int count);

/* The page fading out at the bottom, and the button hints right-aligned on
 * the hint bar at `alpha`. */
void draw_twitch_hints(hui::gfx::DrawList &list, const Context &ctx, const hui::ui::Hint *hints, int count,
                       float alpha);

/* The hints while the rail has the cursor (the page takes no buttons then):
 * what Cross and Circle do on the rail. Fills out[0..1], returns 2. */
int rail_hints(hui::ui::Hint *out);

/* "Watch live" at r (tw::kWatchW x tw::kWatchH): white with dark ink as
 * `focus` goes to 1, a spinner and "Starting" while `starting`. */
void draw_watch_button(hui::gfx::DrawList &list, const Context &ctx, const hui::gfx::Rect &r, float focus,
                       bool starting);

/* ------------------------------------------------------------------- grids */

/* A grid of cards that fills the content width and scrolls up and down. */
struct TwitchGrid
{
    int cols = 1;
    float top = 0.0f; /* the first row's top, page coordinates */
    float card_w = 0.0f, card_h = 0.0f;
    float row_gap = tw::kRowGap; /* card bottom to the next row's card */
    float text_h = tw::kTextH;   /* the lines under a card */

    /* `cols` cards to a row from tw::kLeft to tw::kRight; aspect is height / width. */
    static TwitchGrid make(int cols, float aspect, float top, float row_gap = tw::kRowGap,
                           float text_h = tw::kTextH);
    float pitch() const { return card_h + row_gap; }
    int rows(int count) const { return count > 0 ? (count + cols - 1) / cols : 0; }
    /* Card `index`, page coordinates. */
    hui::gfx::Rect cell(int index) const;
    /* How far the page scrolls with the cursor on `index` of `count`: the
     * first row never moves it, nor does anything in a grid that fits on the
     * screen; otherwise the cursor's row sits second from the top, with the
     * row above it in full and the next one under it. */
    float page_for(int index, int count) const;
};

/* The band the hint bar sits on, for a page with cards: what scrolls down to
 * it fades out and nothing shows behind the hints. Draw it before the hints. */
void draw_hint_band(hui::gfx::DrawList &list, const Context &ctx);

/* Where that band starts: lines that must be read end above it. */
constexpr float kHintBandTop = 972.0f;

/* What moves on a grid screen: the page, the focus ring, and the switch from
 * skeletons to cards. */
struct TwitchGridMotion
{
    float age = 0.0f;
    hui::ui::SpringRect ring;
    bool ring_snapped = false;
    hui::tween::Spring ring_alpha;
    hui::tween::Spring page;
    hui::tween::Spring in; /* skeletons 0 -> cards 1 */

    void enter(float page_target, bool ready);
    /* ring_target is on screen, where the ring lands once the page stops;
     * w == 0 when no card has the cursor. */
    void update(float dt, float page_target, const hui::gfx::Rect &ring_target, bool ready);
};

/* --------------------------------------------------------------------- Home */

/* Home: hero, "Following · live now" (logged in, someone live), "Recently
 * watched" (channels watched live), "Continue watching" (videos with saved
 * progress), "Popular now", "Top categories". */
class TwitchHomeScreen
{
  public:
    void set(const evo_hui_twitch_home_params_t &params);
    void enter();
    void update(float dt);
    void draw(hui::gfx::DrawList &list, const Context &ctx) const;
    bool focus(FocusInfo *out) const;

  private:
    int row_count() const { return static_cast<int>(streams_.size()) > 1 ? static_cast<int>(streams_.size()) - 1 : 0; }
    int followed_count() const { return followed_shown_ ? static_cast<int>(followed_.size()) : 0; }
    int continue_count() const { return continue_shown_ ? static_cast<int>(continue_.size()) : 0; }
    int recent_count() const { return recent_shown_ ? static_cast<int>(recent_.size()) : 0; }
    /* How far a row sits below its place without the optional rows above it
     * (Following, Recently watched, Continue watching): now (they slide in
     * and out), or once they have (target). Popular now and Top categories
     * sit below theirs by all three (shift()). */
    float shift() const;
    float shift_target() const;
    float recent_shift() const;
    float recent_shift_target() const;
    float continue_shift() const;
    float continue_shift_target() const;
    /* A row's title baseline once the rows have slid (page coordinates), and
     * where it starts and ends for bringing it into view. */
    float row_title_target(int row) const;
    float row_bottom_target(int row) const;
    hui::gfx::Rect followed_rect(int col) const; /* page coordinates, scrolled sideways */
    hui::gfx::Rect recent_rect(int col) const;   /* the avatar's circle */
    hui::gfx::Rect continue_rect(int col) const;
    hui::gfx::Rect stream_rect(int col) const;
    hui::gfx::Rect category_rect(int col) const;
    hui::gfx::Rect focus_target() const;         /* where the ring goes, on screen */
    float page_target() const;

    /* hero_tex: the hero picture's texture, looked up once per draw. */
    void draw_hero(hui::gfx::DrawList &list, const Context &ctx, float alpha, std::uint32_t hero_tex) const;
    void draw_hero_skeleton(hui::gfx::DrawList &list, const Context &ctx, float alpha) const;
    void draw_followed(hui::gfx::DrawList &list, const Context &ctx, float alpha) const;
    void draw_recent(hui::gfx::DrawList &list, const Context &ctx, float alpha) const;
    void draw_continue(hui::gfx::DrawList &list, const Context &ctx, float alpha) const;
    void draw_streams(hui::gfx::DrawList &list, const Context &ctx, float alpha) const;
    void draw_categories(hui::gfx::DrawList &list, const Context &ctx, float alpha) const;

    int streams_status_ = EVO_HUI_TWITCH_LOADING;
    int categories_status_ = EVO_HUI_TWITCH_LOADING;
    std::string streams_error_, categories_error_;
    std::vector<TwitchStream> streams_;
    std::vector<TwitchCategory> categories_;
    TwitchPicture hero_art_;
    int focus_row_ = EVO_HUI_TWITCH_ROW_HERO;
    int stream_col_ = 0, category_col_ = 0;
    bool rail_focused_ = false;
    std::string starting_;
    bool followed_shown_ = false;
    std::vector<TwitchStream> followed_;
    int followed_col_ = 0;
    bool continue_shown_ = false;
    std::vector<TwitchVideo> continue_;
    int continue_col_ = 0;
    std::string starting_id_;
    bool recent_shown_ = false;
    std::vector<TwitchChannel> recent_;
    int recent_col_ = 0;

    float age_ = 0.0f;
    hui::ui::SpringRect ring_;
    bool ring_snapped_ = false;
    hui::tween::Spring ring_alpha_;
    hui::tween::Spring hero_focus_;
    hui::tween::Spring followed_in_; /* 1 once the followed row has slid in */
    hui::tween::Spring continue_in_; /* and the continue row */
    hui::tween::Spring recent_in_;   /* and the recently watched row */
    hui::tween::Spring ring_round_;  /* 1 rings an avatar (a circle), 0 a card */
    hui::ui::Scroller followed_scroll_, recent_scroll_, continue_scroll_, stream_scroll_, category_scroll_;
    hui::tween::Spring page_;
    hui::tween::Spring streams_in_, categories_in_; /* skeleton -> content */
    /* The hero's lines, worked out again when streams_[0]'s text changes. */
    struct HeroLines
    {
        const void *fonts = nullptr;
        std::uint32_t rev = ~0u;
        std::string login, name, game, watching;
        std::vector<std::string> title;
    };
    mutable HeroLines hero_lines_;
    CardMotion hero_motion_;
    CardMotion stream_motion_[EVO_HUI_TWITCH_HOME_STREAMS];
    CardMotion category_motion_[EVO_HUI_TWITCH_HOME_CATEGORIES];
    CardMotion followed_motion_[EVO_HUI_TWITCH_HOME_FOLLOWED];
    CardMotion continue_motion_[EVO_HUI_TWITCH_HOME_CONTINUE];
    CardMotion recent_motion_[EVO_HUI_TWITCH_HOME_RECENT];
};

/* ------------------------------------------------------------------- Browse */

/* Browse: "Browse" over a grid of the top categories, six to a row. */
class TwitchBrowseScreen
{
  public:
    void set(const evo_hui_twitch_browse_params_t &params);
    void enter();
    void update(float dt);
    void draw(hui::gfx::DrawList &list, const Context &ctx) const;
    bool focus(FocusInfo *out) const;

  private:
    static TwitchGrid grid();
    bool ready() const { return status_ == EVO_HUI_TWITCH_READY && !categories_.empty(); }
    hui::gfx::Rect ring_target() const;

    int status_ = EVO_HUI_TWITCH_LOADING;
    std::string error_;
    std::vector<TwitchCategory> categories_;
    int cursor_ = 0;
    bool rail_focused_ = false;

    TwitchGridMotion motion_;
    CardMotion cards_[EVO_HUI_TWITCH_BROWSE_CATEGORIES];
};

/* ----------------------------------------------------------------- Category */

/* One category: box art, name and viewers, the Live and Clips tabs, then the
 * open tab's grid four to a row: its live streams, or the clip chips over its
 * clips. One ring runs through the tabs, the chips and the cards. */
class TwitchCategoryScreen
{
  public:
    void set(const evo_hui_twitch_category_params_t &params);
    void enter();
    void update(float dt);
    void draw(hui::gfx::DrawList &list, const Context &ctx) const;
    bool focus(FocusInfo *out) const;

  private:
    bool on_clips() const { return tab_ == EVO_HUI_TWITCH_CATEGORY_TAB_CLIPS; }
    TwitchGrid grid() const; /* the open tab's */
    int count() const;       /* its cards */
    int grid_cursor() const;
    int list_status() const;
    float page_target() const;
    hui::gfx::Rect ring_target() const; /* on screen, once the page stops; w == 0 for none */
    void draw_header(hui::gfx::DrawList &list, const Context &ctx, float page) const;
    void draw_empty(hui::gfx::DrawList &list, const Context &ctx) const;

    std::string name_;
    int viewers_ = -1;
    FittedText name_fit_, empty_fit_;
    TwitchPicture art_;
    CardMotion art_motion_;
    int status_ = EVO_HUI_TWITCH_LOADING;
    std::string error_;
    std::vector<TwitchStream> streams_;
    int cursor_ = 0;
    bool rail_focused_ = false;
    std::string starting_;
    int tab_ = EVO_HUI_TWITCH_CATEGORY_TAB_LIVE;
    int focus_ = EVO_HUI_TWITCH_FOCUS_GRID;
    int filter_ = EVO_HUI_TWITCH_CLIPS_WEEK, filter_cursor_ = 0;
    int clips_status_ = EVO_HUI_TWITCH_LOADING;
    std::string clips_error_;
    std::vector<TwitchVideo> clips_;
    int clip_cursor_ = 0;
    std::string starting_id_;

    TwitchGridMotion motion_;
    hui::tween::Spring ring_round_; /* 1 rings a tab or a chip (a pill), 0 a card */
    hui::tween::Spring tab_line_;   /* the open tab's line, as a tab index */
    hui::tween::Spring tabs_focus_; /* 1 while the tabs have the cursor */
    int shown_tab_ = -1;            /* the tab update() last saw: a switch shows its own grid at once */
    CardMotion cards_[EVO_HUI_TWITCH_CATEGORY_STREAMS];
    CardMotion clip_cards_[EVO_HUI_TWITCH_CATEGORY_CLIPS];
};

/* ------------------------------------------------------------------- Search */

/* Search: the field, then the channels and the categories for the query. */
class TwitchSearchScreen
{
  public:
    void set(const evo_hui_twitch_search_params_t &params);
    void enter();
    void update(float dt);
    void draw(hui::gfx::DrawList &list, const Context &ctx) const;
    bool focus(FocusInfo *out) const;

  private:
    /* What goes under the field. */
    enum class Body
    {
        none,    /* no query: the field alone */
        recent,  /* no query: the field and the recent searches */
        rows,    /* the two rows (loading, results, or one of them empty) */
        failed,  /* nothing to show and something failed */
        nothing, /* both came back empty */
    };
    Body body() const;
    hui::gfx::Rect avatar_rect(int col) const; /* page coordinates, scrolled sideways */
    hui::gfx::Rect category_rect(int col) const;
    hui::gfx::Rect recent_rect(int index) const;
    hui::gfx::Rect focus_target() const; /* where the ring goes */

    void draw_field(hui::gfx::DrawList &list, const Context &ctx, float alpha) const;
    void draw_recents(hui::gfx::DrawList &list, const Context &ctx, float alpha) const;
    void draw_channels(hui::gfx::DrawList &list, const Context &ctx, float alpha) const;
    void draw_categories(hui::gfx::DrawList &list, const Context &ctx, float alpha) const;

    std::string query_;
    FittedText query_fit_, none_fit_;
    int channels_status_ = EVO_HUI_TWITCH_LOADING;
    int categories_status_ = EVO_HUI_TWITCH_LOADING;
    std::string channels_error_, categories_error_;
    std::vector<TwitchChannel> channels_;
    std::vector<TwitchCategory> categories_;
    int focus_row_ = EVO_HUI_TWITCH_SEARCH_FIELD;
    int channel_col_ = 0, category_col_ = 0;
    bool rail_focused_ = false;
    std::vector<std::string> recents_;
    int recent_cursor_ = 0;
    mutable FittedText recent_fit_[EVO_HUI_TWITCH_SEARCH_RECENTS];

    float age_ = 0.0f;
    hui::ui::SpringRect ring_;
    bool ring_snapped_ = false;
    hui::tween::Spring ring_alpha_;
    hui::tween::Spring ring_round_; /* 1 rings an avatar (a circle), 0 a card */
    hui::tween::Spring field_focus_;
    hui::ui::Scroller channel_scroll_, category_scroll_;
    hui::tween::Spring channels_in_, categories_in_; /* skeleton -> content */
    hui::tween::Spring recent_focus_[EVO_HUI_TWITCH_SEARCH_RECENTS]; /* each one lit while it has the cursor */
    CardMotion channel_motion_[EVO_HUI_TWITCH_SEARCH_CHANNELS];
    CardMotion category_motion_[EVO_HUI_TWITCH_SEARCH_CATEGORIES];
};

/* ------------------------------------------------------------------ Channel */

/* One channel: the profile, its live stream (or OFFLINE beside the
 * followers), then the Videos and Clips tabs, the open tab's filter chips and
 * its cards four to a row. One ring runs through the tabs, the chips and the
 * cards (Watch live lights up instead), and the page scrolls to keep it in
 * view. */
class TwitchChannelScreen
{
  public:
    void set(const evo_hui_twitch_channel_params_t &params);
    void enter();
    void update(float dt);
    void draw(hui::gfx::DrawList &list, const Context &ctx) const;
    bool focus(FocusInfo *out) const;

  private:
    bool ready() const { return status_ == EVO_HUI_TWITCH_READY; }
    bool on_clips() const { return tab_ == EVO_HUI_TWITCH_CHANNEL_TAB_CLIPS; }
    /* Where the tabs, the chips and the grid go, page coordinates: under the
     * live stream, or right under the profile while offline. */
    float tabs_top() const;
    float chips_top() const;
    TwitchGrid grid() const;
    int chip_count() const;
    float chip_w() const;
    hui::gfx::Rect watch_rect() const; /* where Watch live is for the lines drawn last */
    float page_target() const;
    hui::gfx::Rect ring_target() const; /* on screen, once the page stops; w == 0 for none */

    void draw_header(hui::gfx::DrawList &list, const Context &ctx, float alpha) const;
    void draw_schedule(hui::gfx::DrawList &list, const Context &ctx, float alpha) const;
    void draw_live(hui::gfx::DrawList &list, const Context &ctx, float alpha, std::uint32_t preview_tex) const;
    void draw_tabs(hui::gfx::DrawList &list, const Context &ctx, float alpha) const;
    void draw_cards(hui::gfx::DrawList &list, const Context &ctx, float alpha, float page) const;
    void draw_loading(hui::gfx::DrawList &list, const Context &ctx, float alpha) const;

    int status_ = EVO_HUI_TWITCH_LOADING;
    std::string error_;
    bool error_gone_ = false; /* nothing to try again: the message points back */
    std::string login_, name_, description_, title_, game_;
    int followers_ = -1, viewers_ = 0;
    bool live_ = false;
    TwitchPicture avatar_, preview_;
    int focus_ = EVO_HUI_TWITCH_FOCUS_NONE;
    bool rail_focused_ = false;
    bool starting_ = false;
    std::uint32_t rev_ = 0; /* goes up whenever the text changes */
    int tab_ = EVO_HUI_TWITCH_CHANNEL_TAB_VIDEOS;
    int filter_ = 0, filter_cursor_ = 0;
    int cards_status_ = EVO_HUI_TWITCH_LOADING;
    std::string cards_error_;
    std::vector<TwitchVideo> cards_;
    int cursor_ = 0;
    bool more_loading_ = false;
    std::string starting_id_;
    std::string uptime_;
    std::vector<std::string> tags_;
    struct Scheduled
    {
        std::string when, title;
    };
    std::vector<Scheduled> schedule_;

    float age_ = 0.0f;
    hui::tween::Spring in_; /* skeleton -> page */
    hui::tween::Spring watch_focus_;
    hui::ui::SpringRect ring_;
    bool ring_snapped_ = false;
    hui::tween::Spring ring_alpha_;
    hui::tween::Spring ring_round_; /* 1 rings a tab or a chip (a pill), 0 a card */
    hui::tween::Spring page_;
    hui::tween::Spring cards_in_;   /* the cards' skeletons -> cards */
    hui::tween::Spring tab_line_;   /* the open tab's line, as a tab index */
    hui::tween::Spring tabs_focus_; /* 1 while the tabs have the cursor */
    int shown_list_ = -1;           /* the tab and chip update() last saw: another list starts from skeletons */
    CardMotion avatar_motion_, preview_motion_;
    CardMotion card_motion_[EVO_HUI_TWITCH_CHANNEL_CARDS];
    /* The text, cut and wrapped again only when rev_ or the fonts change. */
    struct Lines
    {
        const void *fonts = nullptr;
        std::uint32_t rev = ~0u;
        std::string name, followers, game, watching;
        std::vector<std::string> description, title;
        std::vector<std::string> tags; /* the ones that fit on the line, cut to fit */
        std::vector<float> tag_w;
        std::vector<std::string> schedule; /* the titles, cut to fit */
    };
    mutable Lines lines_;
};

/* ---------------------------------------------------------------- Following */

/* Following: "Live now", the followed channels that are live as stream cards
 * four to a row, then "Offline", the rest as round avatars with their names
 * seven to a row. One cursor runs through both grids and the page scrolls
 * to keep its row in view, as on a category; the ring is a circle on an
 * avatar. Logged out, one message says how to log in instead. */
class TwitchFollowingScreen
{
  public:
    void set(const evo_hui_twitch_following_params_t &params);
    void enter();
    void update(float dt);
    void draw(hui::gfx::DrawList &list, const Context &ctx) const;
    bool focus(FocusInfo *out) const;

  private:
    /* What goes under the header. */
    enum class Body
    {
        grids,      /* the two sections (loading, cards, or a note in one of them) */
        logged_out, /* how to log in */
        failed,     /* nothing to show and something failed */
        none,       /* the user follows nobody */
    };
    /* Where the two sections go for what set() handed over, page coordinates. */
    struct Layout
    {
        TwitchGrid live;         /* the stream cards */
        int live_rows = 0;       /* rows of cards, or one of skeletons; 0: a note on the title line */
        float live_title = 0.0f; /* baselines */
        float offline_title = 0.0f;
        float avatars_top = 0.0f;
        int avatar_rows = 0;     /* the same for the avatars */
        float end = 0.0f;        /* the bottom of the last line */
    };
    Body body() const;
    bool live_loading() const;
    bool channels_loading() const;
    Layout layout() const;
    /* Avatar `index`, page coordinates: the circle, centred in its column. */
    hui::gfx::Rect avatar_rect(const Layout &l, int index) const;
    /* The cursor's row in both grids together (live rows first), and a row's top. */
    int cursor_row() const;
    float row_top(const Layout &l, int row) const;
    float page_target(const Layout &l) const;
    hui::gfx::Rect ring_target(const Layout &l) const; /* on screen, once the page stops */
    bool on_live() const { return cursor_ < static_cast<int>(live_.size()); }
    int card_count() const { return static_cast<int>(live_.size() + channels_.size()); }

    void draw_header(hui::gfx::DrawList &list, const Context &ctx, float page) const;
    void draw_live(hui::gfx::DrawList &list, const Context &ctx, const Layout &l, float page) const;
    void draw_offline(hui::gfx::DrawList &list, const Context &ctx, const Layout &l, float page) const;

    int login_state_ = EVO_HUI_TWITCH_FOLLOWING_CHECKING;
    int live_status_ = EVO_HUI_TWITCH_LOADING;
    int channels_status_ = EVO_HUI_TWITCH_LOADING;
    std::string live_error_, channels_error_;
    std::vector<TwitchStream> live_;
    std::vector<TwitchChannel> channels_;
    int cursor_ = 0;
    bool rail_focused_ = false;
    bool scope_missing_ = false;
    std::string starting_;
    std::string counts_;     /* "3 live · 42 channels", "" until both lists are in */
    int counts_for_[2] = {-1, -1};

    float age_ = 0.0f;
    hui::ui::SpringRect ring_;
    bool ring_snapped_ = false;
    hui::tween::Spring ring_alpha_;
    hui::tween::Spring ring_round_; /* 1 rings an avatar (a circle), 0 a card */
    hui::tween::Spring page_;
    hui::tween::Spring live_in_, channels_in_; /* skeletons -> cards */
    CardMotion live_motion_[EVO_HUI_TWITCH_FOLLOWING_LIVE];
    CardMotion channel_motion_[EVO_HUI_TWITCH_FOLLOWING_CHANNELS];
};

/* -------------------------------------------------------------------- Login */

/* Logging in: the title and the two steps on the left with the code in large
 * letters, the QR on a white tile on the right. No rail and no cursor: Cross
 * and Circle are the only buttons, and the hints say what they do. While a
 * code is on its way the code box spins and the tile is a skeleton; the
 * check of the saved login, a failure or the login itself replaces the steps
 * with one centred message. Nothing is allocated per frame: the text that
 * changes is laid out again only when it does. */
class TwitchLoginScreen
{
  public:
    void set(const evo_hui_twitch_login_params_t &params);
    void enter();
    void update(float dt);
    void draw(hui::gfx::DrawList &list, const Context &ctx) const;
    bool focus(FocusInfo *out) const;

  private:
    /* A run of dark modules in one row of the QR: one rect each. */
    struct QrRun
    {
        int row, col, len;
    };
    void set_qr(const unsigned char *dark, int size);
    void clear_qr();
    /* The QR's module size and its tile (quiet zone included), on whole
     * canvas pixels so the modules land on whole screen pixels at 1080p and
     * at 4K. */
    int module_px() const;
    hui::gfx::Rect qr_tile() const;

    void draw_header(hui::gfx::DrawList &list, const Context &ctx, float alpha) const;
    void draw_steps(hui::gfx::DrawList &list, const Context &ctx, float alpha) const;
    void draw_qr(hui::gfx::DrawList &list, const Context &ctx, float alpha) const;
    void draw_checking(hui::gfx::DrawList &list, const Context &ctx, float alpha) const;
    void draw_error(hui::gfx::DrawList &list, const Context &ctx, float alpha) const;
    void draw_done(hui::gfx::DrawList &list, const Context &ctx, float alpha) const;
    /* "The code changes in N min", made again only when N changes. */
    const std::string &expiry_text() const;

    int state_ = EVO_HUI_TWITCH_LOGIN_STARTING;
    std::string code_, name_, login_, error_;
    std::uint32_t rev_ = 0; /* goes up whenever name_, login_ or error_ changes */
    int seconds_left_ = -1;
    int qr_size_ = 0;
    std::vector<unsigned char> qr_;
    std::vector<QrRun> runs_;

    float age_ = 0.0f;
    hui::tween::Spring steps_in_; /* the steps and the QR, while starting or waiting */
    hui::tween::Spring code_in_;  /* the code over the spinner */
    hui::tween::Spring qr_in_;    /* the QR over its skeleton */
    hui::tween::Spring checking_in_, error_in_, done_in_;
    float done_age_ = 0.0f;       /* since the login went through: the tick pops in */
    /* The error's lines and the name, cut again only when rev_ or the fonts change. */
    struct Lines
    {
        const void *fonts = nullptr;
        std::uint32_t rev = ~0u;
        std::vector<std::string> error;
        std::string name;              /* fitted after "Logged in as " */
        float lead_w = 0.0f, name_w = 0.0f;
    };
    mutable Lines lines_;
    const Lines &lines(const Context &ctx) const;
    mutable int expiry_minutes_ = -1; /* what expiry_ says; 0 is "less than a minute" */
    mutable std::string expiry_;
};

/* --------------------------------------------------------------------- Chat */

struct ChatSegment
{
    bool emote = false;
    std::string text; /* the text, or the emote's name */
    TwitchPicture pic;
};

/* One piece of a laid-out chat line: a run of text, or a badge or an emote,
 * placed from the line's top left. */
struct ChatPiece
{
    enum Kind : std::uint8_t
    {
        text,
        badge, /* ref: the badge; drawn as an empty slot until it arrives */
        emote, /* ref: the segment */
    };
    Kind kind = text;
    bool semibold = false;
    float x = 0.0f, y = 0.0f; /* text: the baseline; pictures: the top */
    float w = 0.0f, h = 0.0f; /* pictures */
    float size = 0.0f;        /* text */
    hui::gfx::Color color;
    int ref = -1;
    std::string str;
};

/* Where a line's pieces go, for one mode, text size and set of fonts. Kept
 * until those change or an emote's picture arrives (the picture takes the
 * place of its name, which is wider). */
struct ChatLayout
{
    const void *fonts = nullptr;
    int mode = -1;
    int text_size = -1;
    std::uint64_t pictures = 0;
    float height = 0.0f;
    bool card = false; /* a notice: on a tinted card with a bar down its left */
    std::vector<ChatPiece> pieces;
};

struct ChatLine
{
    std::uint64_t serial = 0;
    int kind = EVO_HUI_TWITCH_CHAT_MESSAGE;
    std::string name, login, system_text;
    std::string reply_to, reply_login; /* a reply's parent; "" otherwise */
    bool mention = false;              /* drawn on a tint; may change while the line stays */
    std::uint32_t color = 0xffffff;
    std::vector<TwitchPicture> badges;
    std::vector<ChatSegment> segments;
    mutable ChatLayout layout;
};

/* Chat in the player, drawn over the video: a panel beside it (side mode)
 * or a dim column over its right side (overlay mode). Lines never change
 * once added, so each is laid out once and kept until it leaves the params.
 * No cursor: chat only shows. */
class TwitchChatScreen
{
  public:
    void set(const evo_hui_twitch_chat_params_t &params);
    void enter() {}
    void update(float dt);
    void draw(hui::gfx::DrawList &list, const Context &ctx) const;
    bool focus(FocusInfo *out) const;

  private:
    void draw_side(hui::gfx::DrawList &list, const Context &ctx) const;
    void draw_overlay(hui::gfx::DrawList &list, const Context &ctx) const;

    int mode_ = EVO_HUI_TWITCH_CHAT_SIDE;
    int text_size_ = EVO_HUI_TWITCH_CHAT_TEXT_MEDIUM;
    hui::gfx::Rect video_; /* on the canvas; w == 0 when not known */
    std::string channel_;
    FittedText channel_fit_;
    std::vector<ChatLine> lines_; /* oldest first, as in the params */
    std::vector<ChatLine> merged_; /* set()'s scratch, kept for its capacity */
};

/* The card for a clip made in the player (evo_hui_twitch_clip_params_t): it
 * rises in when it appears and sinks away when it goes. No cursor. */
class TwitchClipCard
{
  public:
    void set(const evo_hui_twitch_clip_params_t &params);
    void enter();
    void update(float dt);
    void draw(hui::gfx::DrawList &list, const Context &ctx) const;
    bool focus(FocusInfo *out) const;

    /* The card's rect on the canvas, as draw() puts it (also while it moves). */
    hui::gfx::Rect card_rect() const;

  private:
    struct QrRun
    {
        int row, col, len;
    };
    int module_px() const;

    bool visible_ = false;
    float time_left_ = 1.0f;
    hui::gfx::Rect area_{0.0f, 0.0f, 1920.0f, 1080.0f};
    int qr_size_ = 0;
    std::vector<unsigned char> qr_;
    std::vector<QrRun> runs_;
    hui::tween::Spring in_;
};

} // namespace evo::kit

#endif /* EVO_HUI_TWITCH_HPP */
