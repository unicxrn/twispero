/*
 * evo_hui_osd.hpp - the player's on-screen display (playback.rml) on
 * ps5-homebrew-ui: drawn over the video in the same AGC frame.
 *
 * A Twitch stream gets Twitch's own player overlay: the channel at the top
 * left (round avatar ringed red with the LIVE tag under it, the name, the
 * stream title and "Category · 12.3K watching"), the quality playing at the
 * top right and the player's keys at the bottom. A Twitch video or clip has
 * the same top (no red ring, "Category · Past broadcast · 3 days ago") over a
 * file's timeline and keys. Anything else shows its title and metadata, and
 * a timeline unless it is live. Over both: a centre
 * badge for paused / loading / seeking, and the "stats for nerds" panel. A
 * stream with no picture (Twitch's "Audio only") shows the channel large in
 * the middle of the screen instead. The caller's `alpha` fades the chrome.
 */
#ifndef EVO_HUI_OSD_HPP
#define EVO_HUI_OSD_HPP

#include "evo_hui_screens.hpp"
#include "evo_hui_twitch.hpp"

namespace evo::kit
{

class OsdScreen
{
  public:
    void set(const evo_playback_osd_params_t &params);
    void set_hud(const evo_perf_hud_t &hud);
    void enter();
    void update(float dt);
    void draw(hui::gfx::DrawList &list, const Context &ctx) const;
    bool focus(FocusInfo *out) const;

  private:
    /* The text as drawn, cut to its widths: worked out again only when the
     * text (rev_) or the fonts change. */
    struct Lines
    {
        const void *fonts = nullptr;
        std::uint32_t rev = ~0u;
        std::string name, initial;          /* the channel (or the title), and its first letter */
        std::vector<std::string> title;     /* the stream title, one or two lines */
        std::string category, watching;     /* "Category", "12.3K watching" (a video's line) */
        std::string big_name;               /* the audio-only view's name */
        std::vector<std::string> big_title; /* and its title, one or two lines */
        std::string vod_title, vod_meta;
    };
    const Lines &lines(const Context &ctx) const;

    void draw_channel(hui::gfx::DrawList &list, const Context &ctx) const;
    /* "Category · 12.3K watching" on baseline y, from x or centred on it. */
    void draw_meta(hui::gfx::DrawList &list, const Context &ctx, float x, float y, bool centred) const;
    void draw_title(hui::gfx::DrawList &list, const Context &ctx) const;
    void draw_quality(hui::gfx::DrawList &list, const Context &ctx) const;
    void draw_bottom(hui::gfx::DrawList &list, const Context &ctx) const;
    void draw_centre(hui::gfx::DrawList &list, const Context &ctx) const;
    void draw_audio_only(hui::gfx::DrawList &list, const Context &ctx) const;
    void draw_stats(hui::gfx::DrawList &list, const Context &ctx) const;
    /* The avatar as a circle of `radius` at (cx, cy), with the red ring and
     * the LIVE tag (of text size `pill`) while live; the channel's first
     * letter on Twitch purple until the picture is in. */
    void draw_avatar(hui::gfx::DrawList &list, const Context &ctx, float cx, float cy, float radius,
                     float pill) const;

    evo_playback_osd_params_t raw_{}; /* numbers only: its strings dangle */
    std::string title_, meta_, status_, quality_;
    std::string channel_, login_, stream_title_, category_, video_meta_;
    int viewers_ = -1;
    TwitchPicture avatar_;
    CardMotion avatar_motion_;
    std::uint32_t rev_ = 0; /* goes up whenever any text above changes */
    mutable Lines lines_;

    bool hud_on_ = false;
    std::string hud_lines_[5];
    std::vector<float> gpu_hist_, ram_hist_, cpu_hist_;
    float gpu_pct_ = 0, ram_mb_ = 0, ram_total_ = 0, cpu_pct_ = 0;

    float time_ = 0.0f;
    hui::tween::Spring chrome_;
    hui::tween::Spring paused_;
    hui::tween::Spring loading_;
    hui::tween::Spring scrub_;
    hui::tween::Spring progress_;
    bool progress_snapped_ = false;
};

} // namespace evo::kit

#endif /* EVO_HUI_OSD_HPP */
