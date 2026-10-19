#include "evo/screens/TwitchHomeScreen.hpp"
#include "evo/screens/TwitchCategoryScreen.hpp"
#include "evo/screens/TwitchChannelScreen.hpp"
#include "evo/screens/TwitchScreenData.hpp"
#include "evo_twitch_settings.h"
#include "evo/Application.hpp"
#include "evo_rmlui_bridge.h"

#include "twitch/progress.hpp"

#include <algorithm>
#include <cstring>

namespace evo {

namespace {

using twitch_screen::due;
using twitch_screen::kHeroH;
using twitch_screen::kHeroW;

/* Pictures asked for beyond the cards a row draws: two before, three after. */
constexpr int kPicturesBefore = 2, kPicturesAfter = 3;
/* Continue watching's are video thumbnails nothing else on Home shows: only
 * the cards that can be on screen and one either side. */
constexpr int kContinuePicturesBefore = 1, kContinuePicturesAfter = 1;

/* The rows top to bottom, as Up and Down go through them. */
constexpr int kRowOrder[] = {EVO_HUI_TWITCH_ROW_HERO,     EVO_HUI_TWITCH_ROW_FOLLOWED, EVO_HUI_TWITCH_ROW_RECENT,
                             EVO_HUI_TWITCH_ROW_CONTINUE, EVO_HUI_TWITCH_ROW_STREAMS,  EVO_HUI_TWITCH_ROW_CATEGORIES};
constexpr int kRows = static_cast<int>(sizeof kRowOrder / sizeof kRowOrder[0]);

int rowPlace(int row) {
    for (int i = 0; i < kRows; ++i)
        if (kRowOrder[i] == row) return i;
    return 0;
}

/* A video's type as its progress keeps it, for the launcher. */
int videoTypeOf(const std::string& type) {
    if (type == "highlight") return EVO_TWITCH_VIDEO_HIGHLIGHT;
    if (type == "upload") return EVO_TWITCH_VIDEO_UPLOAD;
    return EVO_TWITCH_VIDEO_ARCHIVE;
}

/* Live previews come as ".../live_user_<login>-440x248.jpg?t=..."; the hero
 * asks for the same picture at 800x450. Empty when the URL has another shape. */
std::string heroUrl(const std::string& thumb) {
    return twitch_screen::resized(thumb, "-440x248.", "-800x450.");
}

} // namespace

TwitchHomeScreen::TwitchHomeScreen()
    : StatefulScreen("TwitchHomeScreen") {
    /* The kit reads m_row as the params' focus_row. */
    static_assert(Hero == EVO_HUI_TWITCH_ROW_HERO, "Row must match EVO_HUI_TWITCH_ROW_*");
    static_assert(Followed == EVO_HUI_TWITCH_ROW_FOLLOWED, "Row must match EVO_HUI_TWITCH_ROW_*");
    static_assert(Streams == EVO_HUI_TWITCH_ROW_STREAMS, "Row must match EVO_HUI_TWITCH_ROW_*");
    static_assert(Categories == EVO_HUI_TWITCH_ROW_CATEGORIES, "Row must match EVO_HUI_TWITCH_ROW_*");
    static_assert(Continue == EVO_HUI_TWITCH_ROW_CONTINUE, "Row must match EVO_HUI_TWITCH_ROW_*");
    static_assert(Recent == EVO_HUI_TWITCH_ROW_RECENT, "Row must match EVO_HUI_TWITCH_ROW_*");
}

void TwitchHomeScreen::onExit() {
    evo_twitch_preview_stop();
    StatefulScreen::onExit();
}

void TwitchHomeScreen::onEnter() {
    StatefulScreen::onEnter();
    m_enteredMs = m_lastInputMs = evo_twitch::data_now_ms();
    m_streamsToast.sync(evo_twitch::home_streams());
    m_categoriesToast.sync(evo_twitch::home_categories());
    m_followedToast.sync(evo_twitch::followed_live());
    followLogin();
    refresh(false);
}

void TwitchHomeScreen::update(double deltaMs) {
    StatefulScreen::update(deltaMs);
    followLogin();
    syncPreview();
}

void TwitchHomeScreen::syncPreview() {
    /* None with Settings' switch off, with nothing live, or once the pad has
     * been left alone for a while: a page left up doesn't stream on (and
     * keep the GPU busy) for hours. Any press brings it back. A quick pass
     * through Home starts nothing. */
    const auto& streams = evo_twitch::home_streams().value;
    const unsigned long long now = evo_twitch::data_now_ms();
    /* The rail takes the pad before this screen sees it: open, it counts as
     * being used. */
    if (auto sm = Application::getInstance().getScreenManager())
        if (sm->isRailFocused()) m_lastInputMs = now;
    if (streams.empty() || !evo_twitch::settings().homePreview || twitch_screen::fallbackActive() ||
        now - m_lastInputMs >= kPreviewIdleMs) {
        evo_twitch_preview_stop();
        return;
    }
    if (now - m_enteredMs < kPreviewSettleMs) return;
    evo_twitch_preview(streams[0].login.c_str());
}

void TwitchHomeScreen::followLogin() {
    const evo_twitch::AuthView& v = evo_twitch::auth_view();
    const std::string none;
    const std::string& id = v.state == evo_twitch::AuthState::LoggedIn ? v.userId : none;
    if (id == m_userId) return;
    m_userId = id;
    m_followedCol = 0;
    m_followedToast.sync(evo_twitch::followed_live());
    if (!m_userId.empty() && due(evo_twitch::followed_live(), evo_twitch::data_now_ms())) {
        evo_twitch::request_followed_live(m_userId);
        m_outage.newRound();
    }
}

void TwitchHomeScreen::refresh(bool force) {
    const unsigned long long now = evo_twitch::data_now_ms();
    bool asked = false;
    if (force || due(evo_twitch::home_streams(), now)) {
        evo_twitch::request_home_streams();
        asked = true;
    }
    if (force || due(evo_twitch::home_categories(), now)) {
        evo_twitch::request_home_categories();
        asked = true;
    }
    /* A login without the follows scope asks for nothing (the data service
     * keeps the Error until another login). */
    if (!m_userId.empty() && (force || due(evo_twitch::followed_live(), now))) {
        evo_twitch::request_followed_live(m_userId);
        asked = true;
    }
    if (asked) m_outage.newRound();
}

int TwitchHomeScreen::itemCount(int row) const {
    const int streams = static_cast<int>(evo_twitch::home_streams().value.size());
    switch (row) {
        case Hero:       return streams > 0 ? 1 : 0;
        case Followed:
            /* Only while logged in: the slot is the logged-in user's. */
            return m_userId.empty() ? 0
                                    : std::min<int>(static_cast<int>(evo_twitch::followed_live().value.size()),
                                                    EVO_HUI_TWITCH_HOME_FOLLOWED);
        case Continue:
            return std::min<int>(static_cast<int>(evo_twitch::progress_continue().size()),
                                 EVO_HUI_TWITCH_HOME_CONTINUE);
        case Recent:
            return std::min<int>(static_cast<int>(evo_twitch::history_channels().size()), EVO_HUI_TWITCH_HOME_RECENT);
        case Streams:    return streams > 1 ? streams - 1 : 0;
        case Categories: return static_cast<int>(evo_twitch::home_categories().value.size());
        default:         return 0;
    }
}

void TwitchHomeScreen::keepRowValid() {
    if (itemCount(m_row) > 0) return;
    /* Following, Recently watched or Continue watching went (a logout,
     * nobody live any more, the history cleared, the last video watched to
     * its end): the cursor stays where the rows were, on the next row down,
     * which takes the row's place. */
    const int at = rowPlace(m_row);
    if (m_row == Followed || m_row == Recent || m_row == Continue) {
        for (int i = at + 1; i < kRows; ++i) {
            if (itemCount(kRowOrder[i]) > 0) {
                m_row = kRowOrder[i];
                return;
            }
        }
    }
    for (int d = 1; d < kRows; ++d) {
        if (at - d >= 0 && itemCount(kRowOrder[at - d]) > 0) {
            m_row = kRowOrder[at - d];
            return;
        }
        if (at + d < kRows && itemCount(kRowOrder[at + d]) > 0) {
            m_row = kRowOrder[at + d];
            return;
        }
    }
    /* Nothing anywhere (loading or failed): the cursor waits where it is. */
}

void TwitchHomeScreen::moveRow(int dir) {
    for (int i = rowPlace(m_row) + dir; i >= 0 && i < kRows; i += dir) {
        if (itemCount(kRowOrder[i]) > 0) {
            m_row = kRowOrder[i];
            evo_feedback(EVO_FB_MOVE);
            return;
        }
    }
    evo_feedback(EVO_FB_BOUNDARY);
}

void TwitchHomeScreen::moveCol(int dir) {
    int* col = m_row == Followed     ? &m_followedCol
               : m_row == Recent     ? &m_recentCol
               : m_row == Continue   ? &m_continueCol
               : m_row == Streams    ? &m_streamCol
               : m_row == Categories ? &m_categoryCol
                                     : nullptr;
    const int count = itemCount(m_row);
    if (!col || count <= 0) {
        evo_feedback(EVO_FB_BOUNDARY);
        return;
    }
    const int next = *col + dir;
    if (next < 0 || next >= count) {
        evo_feedback(EVO_FB_BOUNDARY);
        return;
    }
    *col = next;
    evo_feedback(EVO_FB_MOVE);
}

std::string TwitchHomeScreen::focusedLogin() const {
    const auto& streams = evo_twitch::home_streams().value;
    if (m_row == Hero && !streams.empty()) return streams[0].login;
    if (m_row == Followed && m_followedCol < itemCount(Followed))
        return evo_twitch::followed_live().value[static_cast<std::size_t>(m_followedCol)].login;
    if (m_row == Streams && m_streamCol + 1 < static_cast<int>(streams.size()))
        return streams[static_cast<std::size_t>(m_streamCol + 1)].login;
    if (m_row == Continue && m_continueCol < itemCount(Continue))
        return evo_twitch::progress_continue()[static_cast<std::size_t>(m_continueCol)].login;
    if (m_row == Recent && m_recentCol < itemCount(Recent))
        return evo_twitch::history_channels()[static_cast<std::size_t>(m_recentCol)].login;
    return std::string();
}

void TwitchHomeScreen::activate() {
    if (m_row == Continue && m_continueCol < itemCount(Continue)) {
        /* Copied: the list changes when the launcher records progress. */
        const evo_twitch::VideoProgress v = evo_twitch::progress_continue()[static_cast<std::size_t>(m_continueCol)];
        const std::string age = evo_twitch::progress_age(v);
        evo_twitch_video_info_t info{};
        info.type = videoTypeOf(v.type);
        info.title = v.title.c_str();
        info.channel_login = v.login.c_str();
        info.channel_name = v.channel.c_str();
        info.category = v.game.c_str();
        info.age = age.c_str();
        info.published = v.published.c_str();
        info.thumb_url = v.thumb.c_str();
        info.seconds = v.duration;
        info.views = -1;
        if (twitch_screen::playVideo(v.id, info, m_startingId)) m_startingId = v.id;
        return;
    }
    const std::string login = focusedLogin();
    if (m_row == Recent && m_recentCol < itemCount(Recent)) {
        /* Live (as far as Home's lists say): watch it; otherwise its page. */
        const std::vector<int>& viewers = recentViewers();
        const std::size_t i = static_cast<std::size_t>(m_recentCol);
        if (i < viewers.size() && viewers[i] >= 0) {
            if (twitch_screen::play(login, m_startingLogin)) m_startingLogin = login;
        } else {
            evo_feedback(EVO_FB_OPEN);
            TwitchChannelScreen::show(login);
        }
        return;
    }
    if (m_row == Categories && m_categoryCol < itemCount(Categories)) {
        const evo_twitch::CategoryCard& c =
            evo_twitch::home_categories().value[static_cast<std::size_t>(m_categoryCol)];
        evo_feedback(EVO_FB_OPEN);
        TwitchCategoryScreen::show(c.name, c.viewers, c.boxArtUrl);
        return;
    }
    if (m_row == Hero && evo_twitch_preview_take(login.c_str())) {
        evo_feedback(EVO_FB_CONFIRM);
        return;
    }
    if (twitch_screen::play(login, m_startingLogin)) m_startingLogin = login;
}

void TwitchHomeScreen::fillContinue(evo_hui_twitch_home_params_t& p) {
    const std::vector<evo_twitch::VideoProgress>& list = evo_twitch::progress_continue();
    const int n = itemCount(Continue);
    if (n <= 0) return;
    /* "42 min left" for the chips, kept until the progress changes. */
    if (m_continueRevision != evo_twitch::progress_revision() ||
        m_continueLines.size() != static_cast<std::size_t>(n)) {
        m_continueRevision = evo_twitch::progress_revision();
        m_continueLines.clear();
        for (int i = 0; i < n; ++i) {
            const evo_twitch::VideoProgress& v = list[static_cast<std::size_t>(i)];
            m_continueLines.push_back(twitch::timeLeftText(v.seconds, v.duration));
        }
    }
    p.continue_shown = 1;
    p.continue_count = n;
    int from = 0, to = 0;
    m_continueWindow.follow(m_continueCol, n);
    m_continueWindow.range(n, kContinuePicturesBefore, kContinuePicturesAfter, &from, &to);
    for (int i = 0; i < n; ++i) {
        const evo_twitch::VideoProgress& v = list[static_cast<std::size_t>(i)];
        evo_hui_twitch_video_t& o = p.continue_items[i];
        o.id = v.id.c_str();
        o.title = v.title.c_str();
        o.detail = "";
        o.age = v.channel.c_str();
        o.chip = m_continueLines[static_cast<std::size_t>(i)].c_str();
        o.views = -1;
        o.seconds = v.duration;
        o.progress = v.duration > 0 ? std::min(1.0f, static_cast<float>(v.seconds) / static_cast<float>(v.duration))
                                    : -1.0f;
        if (i >= from && i < to)
            o.thumb = m_pictures.get("video/" + v.id, v.thumb, twitch_screen::kThumbW, twitch_screen::kThumbH);
    }
}

const std::vector<int>& TwitchHomeScreen::recentViewers() {
    const std::vector<evo_twitch::HistoryChannel>& history = evo_twitch::history_channels();
    const auto& followed = evo_twitch::followed_live();
    const auto& streams = evo_twitch::home_streams();
    /* The followed list only counts while it is this user's (see itemCount). */
    const unsigned long long followedStamp = m_userId.empty() ? 0 : followed.stamp;
    if (m_recentRevision == evo_twitch::history_revision() && m_recentFollowedStamp == followedStamp &&
        m_recentStreamsStamp == streams.stamp && m_recentUser == m_userId &&
        m_recentViewers.size() == history.size())
        return m_recentViewers;
    m_recentRevision = evo_twitch::history_revision();
    m_recentFollowedStamp = followedStamp;
    m_recentStreamsStamp = streams.stamp;
    m_recentUser = m_userId;
    m_recentViewers.assign(history.size(), -1);
    /* A few dozen names against a hundred or so: a plain search. */
    auto find = [](const std::vector<evo_twitch::StreamCard>& list, const std::string& login) {
        for (const evo_twitch::StreamCard& c : list)
            if (c.login == login) return c.viewers;
        return -1;
    };
    for (std::size_t i = 0; i < history.size(); ++i) {
        int v = m_userId.empty() ? -1 : find(followed.value, history[i].login);
        if (v < 0) v = find(streams.value, history[i].login);
        m_recentViewers[i] = v;
    }
    return m_recentViewers;
}

void TwitchHomeScreen::fillRecent(evo_hui_twitch_home_params_t& p) {
    const std::vector<evo_twitch::HistoryChannel>& list = evo_twitch::history_channels();
    const int n = itemCount(Recent);
    if (n <= 0) return;
    const std::vector<int>& viewers = recentViewers();
    p.recent_shown = 1;
    p.recent_count = n;
    int from = 0, to = 0;
    m_recentWindow.follow(m_recentCol, n);
    m_recentWindow.range(n, kPicturesBefore, kPicturesAfter, &from, &to);
    for (int i = 0; i < n; ++i) {
        const evo_twitch::HistoryChannel& c = list[static_cast<std::size_t>(i)];
        evo_hui_twitch_channel_t& o = p.recent[i];
        o.login = c.login.c_str();
        o.name = c.name.empty() ? c.login.c_str() : c.name.c_str();
        o.viewers = static_cast<std::size_t>(i) < viewers.size() ? viewers[static_cast<std::size_t>(i)] : -1;
        /* The same key and size as Search's and Following's avatars. */
        if (i >= from && i < to)
            o.avatar = m_pictures.get("avatar/" + c.login, twitch_screen::avatarUrl(c.avatarUrl),
                                      twitch_screen::kAvatarW, twitch_screen::kAvatarH);
    }
}

bool TwitchHomeScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released) {
    (void)held;
    (void)released;
    m_lastInputMs = evo_twitch::data_now_ms();
    if (twitch_screen::fallbackActive()) return twitch_screen::fallbackInput(pressed);

    if (pressed & PadButtons::Up) {
        moveRow(-1);
        return true;
    }
    if (pressed & PadButtons::Down) {
        moveRow(1);
        return true;
    }
    if (pressed & PadButtons::Left) {
        const int col = m_row == Followed     ? m_followedCol
                        : m_row == Recent     ? m_recentCol
                        : m_row == Continue   ? m_continueCol
                        : m_row == Streams    ? m_streamCol
                        : m_row == Categories ? m_categoryCol
                                              : 0;
        if (col <= 0) return false;  /* the ScreenManager opens the rail */
        moveCol(-1);
        return true;
    }
    if (pressed & PadButtons::Right) {
        moveCol(1);
        return true;
    }
    if (pressed & PadButtons::Cross) {
        activate();
        return true;
    }
    if (pressed & PadButtons::Triangle) {
        evo_feedback(EVO_FB_CONFIRM);
        refresh(true);
        return true;
    }
    if (pressed & PadButtons::Square) {
        const std::string login = focusedLogin();
        if (login.empty()) {
            evo_feedback(EVO_FB_BOUNDARY);
            return true;
        }
        evo_feedback(EVO_FB_OPEN);
        TwitchChannelScreen::show(login);
        return true;
    }
    if (pressed & PadButtons::Circle) {
        if (m_row != Hero && itemCount(Hero) > 0) {
            m_row = Hero;
            evo_feedback(EVO_FB_CANCEL);
            return true;
        }
        return false;
    }
    return false;
}

void TwitchHomeScreen::render(uint32_t* framebuffer, int width, int height) {
    if (twitch_screen::fallbackActive()) {
        twitch_screen::renderFallback(framebuffer, width, height);
        return;
    }
    const auto& streams = evo_twitch::home_streams();
    const auto& categories = evo_twitch::home_categories();
    const auto& followed = evo_twitch::followed_live();
    /* One toast per outage: the lists tend to fail together. */
    m_streamsToast.check(streams, &m_outage);
    m_categoriesToast.check(categories, &m_outage);
    if (m_userId.empty())
        m_followedToast.sync(followed);
    else
        m_followedToast.check(followed, &m_outage);

    /* A refresh can shorten a list under the cursor. */
    m_followedCol = std::clamp(m_followedCol, 0, std::max(0, itemCount(Followed) - 1));
    m_continueCol = std::clamp(m_continueCol, 0, std::max(0, itemCount(Continue) - 1));
    m_recentCol = std::clamp(m_recentCol, 0, std::max(0, itemCount(Recent) - 1));
    m_streamCol = std::clamp(m_streamCol, 0, std::max(0, itemCount(Streams) - 1));
    m_categoryCol = std::clamp(m_categoryCol, 0, std::max(0, itemCount(Categories) - 1));
    keepRowValid();
    if (!evo_twitch_launcher_starting()) {
        m_startingLogin.clear();
        m_startingId.clear();
    }

    bool railFocused = false;
    if (auto sm = Application::getInstance().getScreenManager()) railFocused = sm->isRailFocused();

    static evo_hui_twitch_home_params_t p;
    std::memset(&p, 0, sizeof(p));

    p.streams_status = twitch_screen::status(streams.status);
    p.streams_error = streams.error.c_str();
    p.stream_count = std::min<int>(static_cast<int>(streams.value.size()), EVO_HUI_TWITCH_HOME_STREAMS);
    /* Pictures only for the cards the kit draws and a few either side:
     * asking keeps them in memory (evo_twitch_images.h). Popular now shows
     * streams 1.., so its card `col` is stream col + 1. */
    int sFrom = 0, sTo = 0;
    m_streamWindow.follow(m_streamCol, p.stream_count - 1);
    m_streamWindow.range(p.stream_count - 1, kPicturesBefore, kPicturesAfter, &sFrom, &sTo);
    for (int i = 0; i < p.stream_count; ++i)
        twitch_screen::fill(p.streams[i], streams.value[static_cast<std::size_t>(i)],
                            i == 0 || (i - 1 >= sFrom && i - 1 < sTo) ? &m_pictures : nullptr);
    if (p.stream_count > 0)
        p.hero_art = m_pictures.get("hero/" + streams.value[0].login, heroUrl(streams.value[0].thumbUrl), kHeroW,
                                    kHeroH);

    /* Following · live now: only while logged in with someone live; with no
     * row, Home is as it was without a login. */
    const int fn = itemCount(Followed);
    if (fn > 0) {
        p.followed_shown = 1;
        p.followed_count = fn;
        int fFrom = 0, fTo = 0;
        m_followedWindow.follow(m_followedCol, fn);
        m_followedWindow.range(fn, kPicturesBefore, kPicturesAfter, &fFrom, &fTo);
        for (int i = 0; i < fn; ++i)
            twitch_screen::fill(p.followed[i], followed.value[static_cast<std::size_t>(i)],
                                i >= fFrom && i < fTo ? &m_pictures : nullptr);
    }

    fillRecent(p);
    fillContinue(p);

    p.categories_status = twitch_screen::status(categories.status);
    p.categories_error = categories.error.c_str();
    p.category_count = std::min<int>(static_cast<int>(categories.value.size()), EVO_HUI_TWITCH_HOME_CATEGORIES);
    int cFrom = 0, cTo = 0;
    m_categoryWindow.follow(m_categoryCol, p.category_count);
    m_categoryWindow.range(p.category_count, kPicturesBefore, kPicturesAfter, &cFrom, &cTo);
    for (int i = 0; i < p.category_count; ++i)
        twitch_screen::fill(p.categories[i], categories.value[static_cast<std::size_t>(i)],
                            i >= cFrom && i < cTo ? &m_pictures : nullptr);
    m_pictures.endFrame();

    /* The rail keeps the row: the page must not move when it opens. */
    p.focus_row = m_row;
    p.rail_focused = railFocused ? 1 : 0;
    p.stream_col = m_streamCol;
    p.category_col = m_categoryCol;
    p.followed_col = m_followedCol;
    p.continue_col = m_continueCol;
    p.recent_col = m_recentCol;
    p.starting_login = m_startingLogin.empty() ? nullptr : m_startingLogin.c_str();
    p.starting_id = m_startingId.empty() ? nullptr : m_startingId.c_str();
    p.live_picture = p.stream_count > 0 ? evo_twitch_preview_picture(streams.value[0].login.c_str()) : 0;
    /* Popular now is in the stream language picked on Browse. */
    const std::string& language = evo_twitch::settings().browseLanguage;
    m_streamsLanguage = language.empty() ? std::string() : evo_twitch::browse_language_label(language);
    p.streams_language = m_streamsLanguage.c_str();

#if defined(EVO_AGC_DEVICE)
    evo_hui_update_twitch_home(&p);
    evo_rmlui_render_kit_screen(evo_hui_render_twitch_home, width, height);
#else
    (void)width;
    (void)height;
#endif
}

} // namespace evo
