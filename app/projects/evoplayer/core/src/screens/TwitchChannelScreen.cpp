#include "evo/screens/TwitchChannelScreen.hpp"
#include "evo/screens/TwitchScreenData.hpp"
#include "evo/Application.hpp"
#include "evo_rmlui_bridge.h"

#include <algorithm>
#include <cstring>

namespace evo {

namespace {
constexpr int kCols = EVO_HUI_TWITCH_VIDEO_COLS;
/* How often the times on the page ("Live for ...", "In 5 h") are made again. */
constexpr unsigned long long kClockTextMs = 10000;
} // namespace

TwitchChannelScreen::TwitchChannelScreen()
    : StatefulScreen("TwitchChannelScreen") {
}

void TwitchChannelScreen::show(const std::string& login) {
    ScreenManager* sm = Application::getInstance().getScreenManager();
    if (!sm) return;
    auto* screen = dynamic_cast<TwitchChannelScreen*>(sm->getScreen(ScreenId::TwitchChannel));
    if (!screen) return;
    /* The rail keeps showing where the user came from: Search, Home,
     * Following, Browse. */
    sm->setChannelSection(sm->getSectionForScreen(sm->getCurrentScreenId()));
    screen->open(login);
    sm->navigateTo(ScreenId::TwitchChannel);
}

void TwitchChannelScreen::open(const std::string& login) {
    m_login = login;
    m_focus = Watch;
    m_tab = VideosTab;
    m_videoFilter = EVO_HUI_TWITCH_VIDEOS_ARCHIVE;
    m_clipFilter = EVO_HUI_TWITCH_CLIPS_WEEK;
    m_filterCursor = 0;
    m_cursor[0] = m_cursor[1] = 0;
}

void TwitchChannelScreen::onEnter() {
    StatefulScreen::onEnter();
    m_refreshToast.sync(evo_twitch::channel_page());
    m_cardsToast[VideosTab].sync(evo_twitch::channel_videos());
    m_cardsToast[ClipsTab].sync(evo_twitch::channel_clips());
    refresh(false);
    /* The list too, without waiting for the page: an offline channel shows
     * its first row at once. */
    refreshCards(false);
}

void TwitchChannelScreen::refresh(bool force) {
    if (m_login.empty()) return;
    if (force || m_requested != m_login ||
        twitch_screen::due(evo_twitch::channel_page(), evo_twitch::data_now_ms())) {
        evo_twitch::request_channel(m_login);
        m_requested = m_login;
    }
}

int TwitchChannelScreen::filterCount() const {
    return m_tab == ClipsTab ? EVO_HUI_TWITCH_CLIP_FILTERS : EVO_HUI_TWITCH_VIDEO_FILTERS;
}

std::string TwitchChannelScreen::cardsKey() const {
    return std::to_string(filter()) + ":" + m_login;
}

bool TwitchChannelScreen::cardsOurs() const {
    return !m_login.empty() && m_cardsRequested[m_tab] == cardsKey();
}

void TwitchChannelScreen::refreshCards(bool force) {
    if (m_login.empty()) return;
    const unsigned long long now = evo_twitch::data_now_ms();
    const bool clips = m_tab == ClipsTab;
    const bool stale = clips ? twitch_screen::due(evo_twitch::channel_clips(), now, twitch_screen::kVideosStaleMs)
                             : twitch_screen::due(evo_twitch::channel_videos(), now, twitch_screen::kVideosStaleMs);
    if (!force && cardsOurs() && !stale) return;
    if (clips)
        evo_twitch::request_channel_clips(m_login, twitch_screen::clipPeriod(m_clipFilter));
    else
        evo_twitch::request_channel_videos(m_login, twitch_screen::videoFilter(m_videoFilter));
    m_cardsRequested[m_tab] = cardsKey();
}

bool TwitchChannelScreen::ready() const {
    return !m_login.empty() && m_requested == m_login &&
           evo_twitch::channel_page().status == evo_twitch::Status::Ready;
}

bool TwitchChannelScreen::live() const {
    return ready() && evo_twitch::channel_page().value.live;
}

int TwitchChannelScreen::cardCount() const {
    if (!cardsOurs()) return 0;
    const std::size_t n = m_tab == ClipsTab ? evo_twitch::channel_clips().value.cards.size()
                                            : evo_twitch::channel_videos().value.cards.size();
    return std::min<int>(static_cast<int>(n), EVO_HUI_TWITCH_CHANNEL_CARDS);
}

void TwitchChannelScreen::keepFocusValid() {
    if (ready() && !live() && m_focus == Watch) m_focus = Tabs;
    const int n = cardCount();
    /* A refresh can shorten the list under the cursor. */
    m_cursor[m_tab] = std::clamp(m_cursor[m_tab], 0, std::max(0, n - 1));
    if (m_focus == Grid && n == 0) {
        m_focus = Filters;
        m_filterCursor = filter();
    }
}

void TwitchChannelScreen::openTab(int tab) {
    m_tab = tab;
    m_filterCursor = filter();
    refreshCards(false);
    evo_feedback(EVO_FB_MOVE);
}

void TwitchChannelScreen::moveGrid(int dx, int dy) {
    int& cursor = m_cursor[m_tab];
    const int next = twitch_screen::gridStep(cursor, cardCount(), kCols, dx, dy);
    if (next < 0) {
        evo_feedback(EVO_FB_BOUNDARY);
    } else {
        cursor = next;
        evo_feedback(EVO_FB_MOVE);
    }
    moreNearEnd();
}

void TwitchChannelScreen::moreNearEnd() {
    const int n = cardCount();
    if (n == 0) return;
    const int rows = (n + kCols - 1) / kCols;
    if (m_cursor[m_tab] / kCols < rows - 2) return;
    /* The service asks for nothing when there is no next page or one is on
     * its way already. */
    if (m_tab == ClipsTab)
        evo_twitch::request_more_channel_clips();
    else
        evo_twitch::request_more_channel_videos();
}

void TwitchChannelScreen::updateClockTexts(const evo_twitch::ChannelPage& page) {
    const unsigned long long tick = evo_twitch::data_now_ms() / kClockTextMs;
    const unsigned long long stamp = evo_twitch::channel_page().stamp;
    if (tick == m_clockTick && stamp == m_clockStamp && m_scheduleWhen.size() == page.schedule.size()) return;
    m_clockTick = tick;
    m_clockStamp = stamp;
    m_uptime = page.live ? evo_twitch::uptime_text(page.startedAt) : std::string();
    m_scheduleWhen.clear();
    for (const evo_twitch::ScheduleItem& item : page.schedule)
        m_scheduleWhen.push_back(evo_twitch::starts_in_text(item.startAt));
}

bool TwitchChannelScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released) {
    (void)held;
    (void)released;
    if (twitch_screen::fallbackActive()) return twitch_screen::fallbackInput(pressed);

    if (pressed & PadButtons::Triangle) {
        evo_feedback(EVO_FB_CONFIRM);
        refresh(true);
        refreshCards(true);
        return true;
    }
    if (pressed & PadButtons::Circle) {
        evo_feedback(EVO_FB_CANCEL);
        if (auto sm = Application::getInstance().getScreenManager()) {
            const int section = sm->getSectionForScreen(ScreenId::TwitchChannel);
            sm->navigateBack(sm->getRootScreenForSection(section));
        }
        return true;
    }
    if (!ready()) {
        /* Loading or failed: Left still opens the rail, nothing else to pick. */
        if (pressed & PadButtons::Left) return false;
        if (pressed & (PadButtons::Up | PadButtons::Down | PadButtons::Right)) {
            evo_feedback(EVO_FB_BOUNDARY);
            return true;
        }
        if (pressed & PadButtons::Cross) {
            evo_feedback(EVO_FB_ERROR);
            return true;
        }
        return false;
    }
    keepFocusValid();
    const int n = cardCount();
    int& cursor = m_cursor[m_tab];

    if (pressed & PadButtons::Up) {
        switch (m_focus) {
            case Grid:
                if (cursor >= kCols) {
                    moveGrid(0, -1);
                    return true;
                }
                m_focus = Filters;
                m_filterCursor = filter();
                break;
            case Filters: m_focus = Tabs; break;
            case Tabs:
                if (!live()) {
                    evo_feedback(EVO_FB_BOUNDARY);
                    return true;
                }
                m_focus = Watch;
                break;
            default: evo_feedback(EVO_FB_BOUNDARY); return true;
        }
        evo_feedback(EVO_FB_MOVE);
        return true;
    }
    if (pressed & PadButtons::Down) {
        switch (m_focus) {
            case Watch: m_focus = Tabs; break;
            case Tabs:
                m_focus = Filters;
                m_filterCursor = filter();
                break;
            case Filters:
                if (n == 0) {
                    evo_feedback(EVO_FB_BOUNDARY);
                    return true;
                }
                m_focus = Grid;
                break;
            default: moveGrid(0, 1); return true;
        }
        evo_feedback(EVO_FB_MOVE);
        return true;
    }
    if (pressed & PadButtons::Left) {
        switch (m_focus) {
            case Tabs:
                if (m_tab == VideosTab) return false;  /* the ScreenManager opens the rail */
                openTab(VideosTab);
                return true;
            case Filters:
                if (m_filterCursor == 0) return false;
                --m_filterCursor;
                evo_feedback(EVO_FB_MOVE);
                return true;
            case Grid:
                if (cursor % kCols == 0) return false;
                moveGrid(-1, 0);
                return true;
            default: return false;
        }
    }
    if (pressed & PadButtons::Right) {
        switch (m_focus) {
            case Tabs:
                if (m_tab == ClipsTab) break;
                openTab(ClipsTab);
                return true;
            case Filters:
                if (m_filterCursor + 1 >= filterCount()) break;
                ++m_filterCursor;
                evo_feedback(EVO_FB_MOVE);
                return true;
            case Grid: moveGrid(1, 0); return true;
            default: break;
        }
        evo_feedback(EVO_FB_BOUNDARY);
        return true;
    }
    if (pressed & PadButtons::Cross) {
        switch (m_focus) {
            case Watch:
                if (twitch_screen::play(m_login, m_startingLogin)) m_startingLogin = m_login;
                return true;
            case Filters:
                if (m_filterCursor == filter()) break;
                (m_tab == ClipsTab ? m_clipFilter : m_videoFilter) = m_filterCursor;
                cursor = 0;
                evo_feedback(EVO_FB_CONFIRM);
                refreshCards(false);
                return true;
            case Grid: {
                if (cursor >= n) break;
                /* The page's channel, for the player's OSD: this one's name and picture. */
                const evo_twitch::ChannelPage& page = evo_twitch::channel_page().value;
                const std::string name = page.login == m_login && !page.displayName.empty() ? page.displayName
                                                                                             : m_login;
                const std::string avatar = page.login == m_login ? twitch_screen::avatarUrl(page.avatarUrl)
                                                                 : std::string();
                const std::size_t i = static_cast<std::size_t>(cursor);
                if (m_tab == ClipsTab) {
                    const evo_twitch::ClipCard& c = evo_twitch::channel_clips().value.cards[i];
                    evo_twitch_video_info_t info = twitch_screen::clipInfo(c, avatar);
                    if (c.channelLogin.empty()) {
                        info.channel_login = m_login.c_str();
                        info.channel_name = name.c_str();
                    }
                    if (twitch_screen::playVideo(c.slug, info, m_startingId)) m_startingId = c.slug;
                } else {
                    const evo_twitch::VideoCard& v = evo_twitch::channel_videos().value.cards[i];
                    const evo_twitch_video_info_t info = twitch_screen::videoInfo(v, m_videoFilter, m_login, name, avatar);
                    if (twitch_screen::playVideo(v.id, info, m_startingId)) m_startingId = v.id;
                }
                return true;
            }
            default: break;
        }
        evo_feedback(EVO_FB_BOUNDARY);
        return true;
    }
    return false;
}

void TwitchChannelScreen::render(uint32_t* framebuffer, int width, int height) {
    if (twitch_screen::fallbackActive()) {
        twitch_screen::renderFallback(framebuffer, width, height);
        return;
    }
    const auto& page = evo_twitch::channel_page();
    if (m_requested == m_login) m_refreshToast.check(page);
    const bool clips = m_tab == ClipsTab;
    if (cardsOurs()) {
        if (clips)
            m_cardsToast[ClipsTab].check(evo_twitch::channel_clips());
        else
            m_cardsToast[VideosTab].check(evo_twitch::channel_videos());
    }
    keepFocusValid();
    if (!evo_twitch_launcher_starting()) {
        m_startingLogin.clear();
        m_startingId.clear();
    }

    bool railFocused = false;
    if (auto sm = Application::getInstance().getScreenManager()) railFocused = sm->isRailFocused();

    static evo_hui_twitch_channel_params_t p;
    std::memset(&p, 0, sizeof(p));
    p.login = m_login.c_str();
    p.focus = EVO_HUI_TWITCH_FOCUS_NONE;
    if (m_login.empty()) {
        /* Only reachable by jumping here with the dev remote. */
        p.status = EVO_HUI_TWITCH_ERROR;
        p.error = "Pick a channel in Search";
        p.error_gone = 1;
    } else if (m_requested != m_login) {
        p.status = EVO_HUI_TWITCH_LOADING;
    } else {
        p.status = twitch_screen::status(page.status);
        /* "Not found" alone says too little on a page of its own. And trying
         * again can't help: the message points back instead. */
        const bool gone = page.error == "Not found";
        p.error = gone ? "Channel not found" : page.error.c_str();
        p.error_gone = gone ? 1 : 0;
    }
    if (ready()) {
        const evo_twitch::ChannelPage& c = page.value;
        p.name = c.displayName.c_str();
        p.description = c.description.c_str();
        p.followers = c.followers;
        p.avatar = m_pictures.get("avatar/" + m_login, twitch_screen::avatarUrl(c.avatarUrl), twitch_screen::kAvatarW,
                                  twitch_screen::kAvatarH);
        p.live = c.live ? 1 : 0;
        updateClockTexts(c);
        if (c.live) {
            p.title = c.title.c_str();
            p.game = c.game.c_str();
            p.viewers = c.viewers;
            p.preview = m_pictures.get("preview/" + m_login, c.thumbUrl, twitch_screen::kHeroW, twitch_screen::kHeroH);
            p.uptime = m_uptime.c_str();
            p.tag_count = std::min<int>(static_cast<int>(c.tags.size()), EVO_HUI_TWITCH_CHANNEL_TAGS);
            for (int i = 0; i < p.tag_count; ++i) p.tags[i] = c.tags[static_cast<std::size_t>(i)].c_str();
        }
        /* The next streams: a time it can say, and the title (or the
         * category) to go with it. */
        for (std::size_t i = 0; i < c.schedule.size() && i < m_scheduleWhen.size() &&
                                p.schedule_count < EVO_HUI_TWITCH_CHANNEL_SCHEDULE;
             ++i) {
            const evo_twitch::ScheduleItem& item = c.schedule[i];
            if (m_scheduleWhen[i].empty()) continue;
            evo_hui_twitch_schedule_t& o = p.schedule[p.schedule_count++];
            o.when = m_scheduleWhen[i].c_str();
            o.title = !item.title.empty() ? item.title.c_str()
                      : !item.category.empty() ? item.category.c_str()
                                               : "Scheduled stream";
        }
        p.focus = m_focus;
    }

    /* The open tab's list. Thumbnails for the rows on screen: the cursor's,
     * the one above and the two below (the first two while the cursor is
     * above the cards). */
    p.tab = m_tab;
    p.filter = filter();
    p.filter_cursor = m_filterCursor;
    p.cursor = m_cursor[m_tab];
    p.cards_status = EVO_HUI_TWITCH_LOADING;
    const int n = cardCount();
    const int row = m_focus == Grid ? p.cursor / kCols : 0;
    const int from = std::max(0, (row - 1) * kCols), to = std::min(n, (std::max(row, 1) + 2) * kCols);
    if (cardsOurs()) {
        if (clips) {
            const auto& r = evo_twitch::channel_clips();
            p.cards_status = twitch_screen::status(r.status);
            p.cards_error = r.error.c_str();
            for (int i = 0; i < n; ++i)
                twitch_screen::fill(p.cards[i], r.value.cards[static_cast<std::size_t>(i)], false,
                                    i >= from && i < to ? &m_pictures : nullptr);
            p.more_loading = evo_twitch::channel_clips_loading_more() ? 1 : 0;
        } else {
            const auto& r = evo_twitch::channel_videos();
            p.cards_status = twitch_screen::status(r.status);
            p.cards_error = r.error.c_str();
            for (int i = 0; i < n; ++i)
                twitch_screen::fill(p.cards[i], r.value.cards[static_cast<std::size_t>(i)],
                                    i >= from && i < to ? &m_pictures : nullptr);
            p.more_loading = evo_twitch::channel_videos_loading_more() ? 1 : 0;
        }
        p.card_count = n;
    }
    m_pictures.endFrame();
    p.rail_focused = railFocused ? 1 : 0;
    p.starting = !m_startingLogin.empty() && m_startingLogin == m_login ? 1 : 0;
    p.starting_id = m_startingId.empty() ? nullptr : m_startingId.c_str();

#if defined(EVO_AGC_DEVICE)
    evo_hui_update_twitch_channel(&p);
    evo_rmlui_render_kit_screen(evo_hui_render_twitch_channel, width, height);
#else
    (void)width;
    (void)height;
#endif
}

} // namespace evo
