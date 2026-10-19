#ifndef EVO_TWITCH_SCREEN_DATA_HPP
#define EVO_TWITCH_SCREEN_DATA_HPP

/*
 * What the Twitch screens (Home, Following, Browse, Category, Search,
 * Channel) share between the data and image services (twitch/include) and
 * the kit's params (evo_hui_twitch.h):
 * when to ask for a list again, how a card's picture is asked for, how a
 * play request starts, and how the cursor moves in a grid. Only for screens
 * built with -Itwitch/include (see the Makefile).
 */

#include "evo/Application.hpp"
#include "evo/Common.hpp"
#include "evo_feedback.h"
#include "evo_hui_twitch.h"
#include "evo_rmlui_bridge.h"
#include "evo_toast.h"
#include "evo_twitch.h"
#include "evo_twitch_data.h"
#include "evo_twitch_history.h"
#include "evo_twitch_images.h"
#include "evo_twitch_progress.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <string>
#include <unordered_map>

namespace evo {
namespace twitch_screen {

/* Lists older than this are fetched again when a screen opens. */
constexpr unsigned long long kStaleMs = 60000;
/* A channel's videos and clips change slowly, and a refresh starts a paged
 * list again from its first page (and the cursor with it): only after this
 * long. */
constexpr unsigned long long kVideosStaleMs = 10 * 60000;

/* Picture sizes asked of the image service: what Twitch sends for stream
 * cards and box art. The same size for the same picture on every screen, so
 * a card seen on one screen is already in memory on the next. */
constexpr int kThumbW = 440, kThumbH = 248;
constexpr int kBoxW = 285, kBoxH = 380;
constexpr int kAvatarW = 150, kAvatarH = 150;
/* A live preview at the size Home's hero and the channel page draw it
 * (800 x 450 boxes): a larger one only costs texture memory. */
constexpr int kHeroW = 800, kHeroH = 450;

/* url with its size part swapped ("-440x248." for "-800x450."), or "" when
 * it has no such part. Twitch puts a picture's size in its file name, so a
 * larger copy of the same picture is one edit away. */
inline std::string resized(const std::string& url, const char* from, const char* to) {
    const std::size_t at = url.find(from);
    if (at == std::string::npos) return std::string();
    std::string out = url;
    out.replace(at, std::char_traits<char>::length(from), to);
    return out;
}

/* Search hands out 144x192 box art and 70x70 avatars; the screens ask for the
 * sizes Browse and the channel page use, so a picture seen on one is already
 * in memory on the other (checked live: box art comes in any size, avatars in
 * 150x150 too). Other URLs come back as they are. */
inline std::string boxArtUrl(const std::string& url) {
    std::string out = resized(url, "-144x192.", "-285x380.");
    return out.empty() ? url : out;
}
inline std::string avatarUrl(const std::string& url) {
    std::string out = resized(url, "-70x70.", "-150x150.");
    return out.empty() ? url : out;
}

inline int status(evo_twitch::Status s) {
    switch (s) {
        case evo_twitch::Status::Ready: return EVO_HUI_TWITCH_READY;
        case evo_twitch::Status::Error: return EVO_HUI_TWITCH_ERROR;
        default:                        return EVO_HUI_TWITCH_LOADING;
    }
}

/* A list that is missing, failed or older than staleMs. */
template <class T>
bool due(const evo_twitch::Result<T>& r, unsigned long long now, unsigned long long staleMs = kStaleMs) {
    switch (r.status) {
        case evo_twitch::Status::Idle:
        case evo_twitch::Status::Error:   return true;
        case evo_twitch::Status::Loading: return false;
        case evo_twitch::Status::Ready:   return now > r.stamp && now - r.stamp > staleMs;
    }
    return false;
}

/* The picture at url, or none yet. Asking keeps it in memory, so ask only for
 * what is on screen or close to it (evo_twitch_images.h). */
inline evo_hui_twitch_image_t picture(const std::string& url, int w, int h) {
    evo_hui_twitch_image_t out{};
    if (url.empty()) return out;
    const evo_twitch::Image img = evo_twitch::image(url.c_str(), w, h);
    out.rgba = img.rgba;
    out.w = img.w;
    out.h = img.h;
    out.id = img.id;
    return out;
}

/* Pictures that outlive a refresh. A live thumbnail's URL changes every five
 * minutes (twitch::liveThumbnailUrl), so after a refresh a card's new picture
 * is still downloading: until it lands, the card keeps the picture it had,
 * found by a key that stays with the card (a login, a category name). The
 * old URL is only looked up, never fetched again. Main thread only. */
class PictureKeeper {
public:
    evo_hui_twitch_image_t get(const std::string& key, const std::string& url, int w, int h) {
        evo_hui_twitch_image_t out = picture(url, w, h);
        if (url.empty()) return out;
        Kept& k = m_kept[key];
        k.frame = m_frame;
        if (out.rgba) {
            if (k.url != url) k.url = url;
            return out;
        }
        if (!k.url.empty() && k.url != url) {
            const evo_twitch::Image old = evo_twitch::image_if_ready(k.url.c_str(), w, h);
            out.rgba = old.rgba;
            out.w = old.w;
            out.h = old.h;
            out.id = old.id;
        }
        return out;
    }
    /* Once per frame, after the get()s: cards gone for a while are forgotten. */
    void endFrame() {
        if (++m_frame % 600 != 0) return;
        for (auto it = m_kept.begin(); it != m_kept.end();)
            it = m_frame - it->second.frame > 600 ? m_kept.erase(it) : std::next(it);
    }

private:
    struct Kept {
        std::string url;  /* the last URL that gave pixels */
        unsigned frame = 0;
    };
    std::unordered_map<std::string, Kept> m_kept;
    unsigned m_frame = 0;
};

/* A stream card for the kit; the strings stay owned by `s`. The thumbnail
 * only with `pictures` (asking keeps it in memory). */
inline void fill(evo_hui_twitch_stream_t& o, const evo_twitch::StreamCard& s, PictureKeeper* pictures) {
    o.login = s.login.c_str();
    o.name = s.displayName.c_str();
    o.title = s.title.c_str();
    o.game = s.game.c_str();
    o.viewers = s.viewers;
    if (pictures) o.thumb = pictures->get(s.login, s.thumbUrl, kThumbW, kThumbH);
}

inline void fill(evo_hui_twitch_category_t& o, const evo_twitch::CategoryCard& c, PictureKeeper* pictures) {
    o.name = c.name.c_str();
    o.viewers = c.viewers;
    if (pictures) o.art = pictures->get(c.name, boxArtUrl(c.boxArtUrl), kBoxW, kBoxH);
}

/* A channel from a search; the avatar keyed by the login. */
inline void fill(evo_hui_twitch_channel_t& o, const evo_twitch::ChannelCard& c, PictureKeeper* pictures) {
    o.login = c.login.c_str();
    o.name = c.displayName.c_str();
    o.viewers = c.liveViewers;
    if (pictures) o.avatar = pictures->get("avatar/" + c.login, avatarUrl(c.avatarUrl), kAvatarW, kAvatarH);
}

/* What a chip (EVO_HUI_TWITCH_VIDEOS_*, EVO_HUI_TWITCH_CLIPS_*) asks the data
 * service for. */
inline evo_twitch::VideoFilter videoFilter(int chip) {
    switch (chip) {
        case EVO_HUI_TWITCH_VIDEOS_HIGHLIGHT: return evo_twitch::VideoFilter::Highlight;
        case EVO_HUI_TWITCH_VIDEOS_UPLOAD:    return evo_twitch::VideoFilter::Upload;
        default:                              return evo_twitch::VideoFilter::Archive;
    }
}
inline evo_twitch::ClipPeriod clipPeriod(int chip) {
    switch (chip) {
        case EVO_HUI_TWITCH_CLIPS_DAY:   return evo_twitch::ClipPeriod::Day;
        case EVO_HUI_TWITCH_CLIPS_MONTH: return evo_twitch::ClipPeriod::Month;
        case EVO_HUI_TWITCH_CLIPS_ALL:   return evo_twitch::ClipPeriod::All;
        default:                         return evo_twitch::ClipPeriod::Week;
    }
}

/* A past broadcast, highlight or upload as a card; the detail line is its
 * category, and a bar along its foot shows how much of it was watched
 * (evo_twitch_progress.h). The thumbnail only with `pictures`, keyed by the
 * video's id. */
inline void fill(evo_hui_twitch_video_t& o, const evo_twitch::VideoCard& v, PictureKeeper* pictures) {
    o.id = v.id.c_str();
    o.title = v.title.c_str();
    o.detail = v.game.c_str();
    o.age = v.age.c_str();
    o.views = v.views;
    o.seconds = v.seconds;
    o.progress = evo_twitch::progress_fraction(v.id);
    if (pictures) o.thumb = pictures->get("video/" + v.id, v.thumbUrl, kThumbW, kThumbH);
}

/* A clip as a card. Its detail line: the category on a channel's page, the
 * channel on a category's (`channel`). */
inline void fill(evo_hui_twitch_video_t& o, const evo_twitch::ClipCard& c, bool channel, PictureKeeper* pictures) {
    o.id = c.slug.c_str();
    o.title = c.title.c_str();
    o.detail = channel ? c.channelName.c_str() : c.game.c_str();
    o.age = c.age.c_str();
    o.views = c.views;
    o.seconds = c.seconds;
    o.progress = -1.0f;
    if (pictures) o.thumb = pictures->get("clip/" + c.slug, c.thumbUrl, kThumbW, kThumbH);
}

/* What the launcher is told of a video or a clip from its card (the OSD
 * shows it; a video's progress keeps some of it). The pointers are the
 * card's and the channel's strings: good while they are. `type` is the
 * Videos tab's chip (EVO_HUI_TWITCH_VIDEOS_*). */
inline int videoType(int chip) {
    switch (chip) {
        case EVO_HUI_TWITCH_VIDEOS_HIGHLIGHT: return EVO_TWITCH_VIDEO_HIGHLIGHT;
        case EVO_HUI_TWITCH_VIDEOS_UPLOAD:    return EVO_TWITCH_VIDEO_UPLOAD;
        default:                              return EVO_TWITCH_VIDEO_ARCHIVE;
    }
}
inline evo_twitch_video_info_t videoInfo(const evo_twitch::VideoCard& v, int chip, const std::string& login,
                                         const std::string& name, const std::string& avatar) {
    evo_twitch_video_info_t info{};
    info.type = videoType(chip);
    info.title = v.title.c_str();
    info.channel_login = login.c_str();
    info.channel_name = name.c_str();
    info.category = v.game.c_str();
    info.age = v.age.c_str();
    info.published = v.published.c_str();
    info.thumb_url = v.thumbUrl.c_str();
    info.avatar_url = avatar.c_str();
    info.seconds = v.seconds;
    info.views = v.views;
    return info;
}
/* A clip: its own channel (a category's clips come from many), or the page's
 * when the card has none. */
inline evo_twitch_video_info_t clipInfo(const evo_twitch::ClipCard& c, const std::string& avatar) {
    evo_twitch_video_info_t info{};
    info.type = EVO_TWITCH_VIDEO_CLIP;
    info.title = c.title.c_str();
    info.channel_login = c.channelLogin.c_str();
    info.channel_name = c.channelName.c_str();
    info.category = c.game.c_str();
    info.age = c.age.c_str();
    info.curator = c.curator.c_str();
    info.thumb_url = c.thumbUrl.c_str();
    info.avatar_url = avatar.c_str();
    info.seconds = c.seconds;
    info.views = c.views;
    return info;
}
/* The info keeps pointers: never into a temporary. */
evo_twitch_video_info_t clipInfo(const evo_twitch::ClipCard& c, std::string&& avatar) = delete;

/* Plays a video (by its id) or a clip (by its slug, info.type
 * EVO_TWITCH_VIDEO_CLIP) the user picked, as play() does a channel: true
 * when this request is the one now starting. `starting` is the id this
 * screen already shows a spinner for. */
inline bool playVideo(const std::string& id, const evo_twitch_video_info_t& info, const std::string& starting) {
    if (id.empty()) {
        evo_feedback(EVO_FB_ERROR);
        return false;
    }
    const bool busy = evo_twitch_launcher_starting() != 0;
    if (busy && id == starting) return false;
    evo_feedback(busy ? EVO_FB_ERROR : EVO_FB_CONFIRM);
    if (info.type == EVO_TWITCH_VIDEO_CLIP)
        evo_twitch_play_clip(id.c_str(), &info);
    else
        evo_twitch_play_video(id.c_str(), &info);
    return !busy && evo_twitch_launcher_starting();
}

/* Plays a channel the user picked. True when this request is the one now
 * starting (the screen shows a spinner on its card until
 * evo_twitch_launcher_starting() drops). `starting` is the stream this screen
 * already shows a spinner for: pressing it again does nothing at all. Any
 * other press while a start runs gets the error cue, and the launcher turns
 * it down with a toast. */
inline bool play(const std::string& login, const std::string& starting) {
    if (login.empty()) {
        evo_feedback(EVO_FB_ERROR);
        return false;
    }
    const bool busy = evo_twitch_launcher_starting() != 0;
    if (busy && login == starting) return false;
    evo_feedback(busy ? EVO_FB_ERROR : EVO_FB_CONFIRM);
    evo_twitch_play(login.c_str());
    return !busy && evo_twitch_launcher_starting();
}

/* One toast per outage for a screen with several lists: they tend to fail
 * together (no connection), but their failures land one by one, seconds
 * apart. The screen starts a round whenever it asks for its lists again;
 * within a round, and within kOutageMs of the last toast, only the first
 * failure is told. */
class OutageToasts {
public:
    void newRound() { m_toasted = false; }
    /* True when a failure may be toasted now (and counts it as told). */
    bool claim(unsigned long long now) {
        if (m_toasted || (m_lastMs != 0 && now - m_lastMs < kOutageMs)) return false;
        m_toasted = true;
        m_lastMs = now ? now : 1;
        return true;
    }

private:
    static constexpr unsigned long long kOutageMs = 2000;
    bool m_toasted = false;
    unsigned long long m_lastMs = 0;
};

/* L1/R1's stream language, on Browse and on a category's Live tab. Home's
 * "Popular now" (and its hero) and every category's live list are in it.
 * A press steps it and says it in a toast at once; Settings take it, the
 * file is written and Home's list is asked for again only once the presses
 * have stopped for kDelayMs, or when the screen is left (apply(true)), so
 * running through the list costs one request, not one a press. */
class LanguageStepper {
public:
    static constexpr unsigned long long kDelayMs = 700;

    /* L1 (next false) or R1 (next true). */
    void step(bool next);
    /* What to show now: the language stepped to, or the one in Settings. */
    std::string shown() const;
    /* Every frame; force on leaving. True on the frame Settings took a new
     * language. */
    bool apply(bool force);

private:
    std::string m_code;
    bool m_pending = false;
    unsigned long long m_dueMs = 0;
};

/* "German streams", or "" for every language: the note beside a list that
 * a language narrows. */
std::string languageNote(const std::string& code);

/* The two above live in TwitchBrowseScreen.cpp, which is built with the
 * core's headers (evo_twitch_settings.h names its AppSettings); this header
 * stays clear of them. */

/* A refresh that failed while the old list stays on screen: one toast with
 * the reason (evo_twitch_data.h clears `error` when a refresh starts, so each
 * failure is a change). A failure that leaves nothing to show is the
 * screen's to draw. sync() on entering, so an old failure is not toasted. */
class RefreshToast {
public:
    template <class T>
    void sync(const evo_twitch::Result<T>& r) { m_seen = r.error; }
    /* True when it toasted. */
    template <class T>
    bool check(const evo_twitch::Result<T>& r) {
        return check(r, nullptr);
    }
    /* The same, but told only when `outage` hasn't told this one already. */
    template <class T>
    bool check(const evo_twitch::Result<T>& r, OutageToasts* outage) {
        if (r.error == m_seen) return false;
        m_seen = r.error;
        if (r.status != evo_twitch::Status::Ready || r.error.empty()) return false;
        if (outage && !outage->claim(evo_twitch::data_now_ms())) return false;
        toast_error(r.error.c_str(), nullptr);
        return true;
    }

private:
    std::string m_seen;
};

/* Which cards of one of Home's rows the kit draws, so pictures are asked for
 * those and a few around them rather than the whole row. The kit scrolls a
 * row by the least distance that keeps the cursor's card a margin inside the
 * view (hui::ui::Scroller::reveal, evo_hui_twitch.h's EVO_HUI_TWITCH_HOME_*);
 * the same steps here, every frame with the cursor the kit gets, land on the
 * same scroll. Main thread only. */
class RowWindow {
public:
    RowWindow(float cardW, float margin) : m_w(cardW), m_margin(margin) {}

    /* Once per frame with the cursor and the row's length, as the kit gets
     * them. An empty row leaves the scroll where it was, as in the kit. */
    void follow(int cursor, int count) {
        if (count <= 0) return;
        const float gap = EVO_HUI_TWITCH_HOME_GAP, view = EVO_HUI_TWITCH_HOME_VIEW_W;
        const float pitch = m_w + gap;
        const float start = static_cast<float>(cursor) * pitch, end = start + m_w;
        const float margin = m_w * m_margin;
        float wanted = m_scroll;
        if (start - margin < wanted) wanted = start - margin;
        if (end + margin > wanted + view) wanted = end + margin - view;
        const float limit = static_cast<float>(count) * pitch - gap;
        wanted = std::min(wanted, std::max(0.0f, limit - view));
        m_scroll = std::max(0.0f, wanted);
    }

    /* The cards drawn once the row stops, with `before` more to their left
     * and `after` more to their right (they come in while the row glides),
     * as [*from, *to) of count. */
    void range(int count, int before, int after, int* from, int* to) const {
        const float pitch = m_w + EVO_HUI_TWITCH_HOME_GAP;
        const float x0 = EVO_HUI_TWITCH_HOME_VIEW_X - m_scroll;  /* card 0's left edge on the canvas */
        const int first = static_cast<int>(std::ceil((EVO_HUI_TWITCH_HOME_DRAWN_LEFT - m_w - x0) / pitch));
        const int last = static_cast<int>(std::floor((EVO_HUI_TWITCH_HOME_DRAWN_RIGHT - x0) / pitch));
        *from = std::max(0, first - before);
        *to = std::max(*from, std::min(count, last + 1 + after));
    }

private:
    float m_w, m_margin;
    float m_scroll = 0.0f;  /* where the kit's scroll is going */
};

/* The cursor in a grid of `count` cards, `cols` to a row, after one step:
 * dx for Left/Right (within the row), dy for Up/Down (Down onto a shorter
 * last row lands on its last card). -1 when there is nowhere to go. */
inline int gridStep(int cursor, int count, int cols, int dx, int dy) {
    if (count <= 0 || cols <= 0 || cursor < 0 || cursor >= count) return -1;
    const int col = cursor % cols;
    if (dx != 0) {
        const int next = cursor + dx;
        if (col + dx < 0 || col + dx >= cols || next >= count) return -1;
        return next;
    }
    const int row = cursor / cols + dy, last = (count - 1) / cols;
    if (row < 0 || row > last) return -1;
    const int next = row * cols + col;
    return next < count ? next : count - 1;
}

/* ---------------------------------------------------- when the kit is down */

/* The kit could not start (evo_rmlui_twitch_fallback_active): every Twitch
 * screen then draws the same RmlUi page instead of its own and hands its
 * input to fallbackInput(). The page's cursor is shared by the Twitch screens,
 * so it stays put when the rail changes the screen underneath. */
inline bool fallbackActive() { return evo_rmlui_twitch_fallback_active() != 0; }

inline int& fallbackFocus() {
    static int focus = 0;  /* 0 Open Settings, 1 Quit */
    return focus;
}

inline void renderFallback(uint32_t* framebuffer, int width, int height) {
    evo_rmlui_render_twitch_fallback(framebuffer, width, height, fallbackFocus());
}

/* Left and Right move between the two buttons; Left on the first one opens
 * the rail, as on every Twitch screen (false: the ScreenManager does it).
 * Cross presses the button, and Circle goes back to the screen before (a
 * channel or category page has no rail entry of its own). The rest does
 * nothing here. */
inline bool fallbackInput(uint32_t pressed) {
    int& focus = fallbackFocus();
    if (pressed & PadButtons::Circle) {
        auto sm = Application::getInstance().getScreenManager();
        evo_feedback(sm && sm->navigateBack() ? EVO_FB_CANCEL : EVO_FB_BOUNDARY);
        return true;
    }
    if (pressed & PadButtons::Left) {
        if (focus == 0) return false;
        focus = 0;
        evo_feedback(EVO_FB_MOVE);
        return true;
    }
    if (pressed & PadButtons::Right) {
        evo_feedback(focus == 1 ? EVO_FB_BOUNDARY : EVO_FB_MOVE);
        focus = 1;
        return true;
    }
    if (pressed & PadButtons::Cross) {
        evo_feedback(EVO_FB_OPEN);
        if (focus == 1) {
            Application::getInstance().requestSoftClose();
        } else if (auto sm = Application::getInstance().getScreenManager()) {
            sm->navigateTo(ScreenId::Settings);
        }
        return true;
    }
    return pressed != 0;
}

}  // namespace twitch_screen
}  // namespace evo

#endif // EVO_TWITCH_SCREEN_DATA_HPP
