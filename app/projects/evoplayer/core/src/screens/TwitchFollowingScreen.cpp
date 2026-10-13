#include "evo/screens/TwitchFollowingScreen.hpp"
#include "evo/screens/TwitchChannelScreen.hpp"
#include "evo/screens/TwitchScreenData.hpp"
#include "evo/Application.hpp"
#include "evo_rmlui_bridge.h"

#include <algorithm>
#include <cstring>

namespace evo {

namespace {

using evo_twitch::AuthState;

constexpr int kLiveCols = EVO_HUI_TWITCH_FOLLOWING_LIVE_COLS;
constexpr int kOfflineCols = EVO_HUI_TWITCH_FOLLOWING_CHANNEL_COLS;

int liveRows(int live) {
    return (live + kLiveCols - 1) / kLiveCols;
}

/* The column of the other grid whose middle is nearest column `col`'s (the
 * grids span the same width). */
int matchColumn(int col, int fromCols, int toCols) {
    return ((2 * col + 1) * toCols) / (2 * fromCols);
}

} // namespace

TwitchFollowingScreen::TwitchFollowingScreen()
    : StatefulScreen("TwitchFollowingScreen") {
}

void TwitchFollowingScreen::onEnter() {
    StatefulScreen::onEnter();
    m_liveToast.sync(evo_twitch::followed_live());
    m_channelsToast.sync(evo_twitch::followed_channels());
    followLogin();
    refresh(false);
}

void TwitchFollowingScreen::update(double deltaMs) {
    StatefulScreen::update(deltaMs);
    followLogin();
}

void TwitchFollowingScreen::followLogin() {
    const evo_twitch::AuthView& v = evo_twitch::auth_view();
    const std::string none;
    const std::string& id = v.state == AuthState::LoggedIn ? v.userId : none;
    if (id == m_userId) return;
    m_userId = id;
    m_cursor = 0;
    m_liveToast.sync(evo_twitch::followed_live());
    m_channelsToast.sync(evo_twitch::followed_channels());
    refresh(false);
}

void TwitchFollowingScreen::refresh(bool force) {
    if (m_userId.empty()) return;
    const unsigned long long now = evo_twitch::data_now_ms();
    const bool live = force || twitch_screen::due(evo_twitch::followed_live(), now);
    const bool channels = force || twitch_screen::due(evo_twitch::followed_channels(), now);
    if (live) evo_twitch::request_followed_live(m_userId);
    if (channels) evo_twitch::request_followed_channels(m_userId);
    if (live || channels) m_outage.newRound();
}

int TwitchFollowingScreen::liveCount() const {
    if (m_userId.empty()) return 0;
    return std::min<int>(static_cast<int>(evo_twitch::followed_live().value.size()), EVO_HUI_TWITCH_FOLLOWING_LIVE);
}

void TwitchFollowingScreen::updateOffline() {
    const auto& live = evo_twitch::followed_live();
    const auto& channels = evo_twitch::followed_channels();
    if (live.stamp == m_liveStamp && channels.stamp == m_channelsStamp && live.value.size() == m_liveSize &&
        channels.value.size() == m_channelsSize)
        return;
    m_liveStamp = live.stamp;
    m_channelsStamp = channels.stamp;
    m_liveSize = live.value.size();
    m_channelsSize = channels.value.size();

    /* Every followed channel shows once: a live card wins over its avatar.
     * The two lists come from two calls, so one can be a little newer. */
    const int live_n = liveCount();
    m_offline.clear();
    for (std::size_t i = 0; i < channels.value.size(); ++i) {
        if (static_cast<int>(m_offline.size()) >= EVO_HUI_TWITCH_FOLLOWING_CHANNELS) break;
        const std::string& login = channels.value[i].login;
        bool isLive = false;
        for (int j = 0; j < live_n && !isLive; ++j)
            isLive = live.value[static_cast<std::size_t>(j)].login == login;
        if (!isLive) m_offline.push_back(static_cast<int>(i));
    }
}

void TwitchFollowingScreen::keepCursorValid() {
    updateOffline();
    const int n = m_userId.empty() ? 0 : liveCount() + offlineCount();
    m_cursor = std::clamp(m_cursor, 0, std::max(0, n - 1));
}

bool TwitchFollowingScreen::focused(std::string* login, bool* live) const {
    const int live_n = liveCount();
    if (m_cursor < live_n) {
        *login = evo_twitch::followed_live().value[static_cast<std::size_t>(m_cursor)].login;
        *live = true;
        return true;
    }
    const int i = m_cursor - live_n;
    if (m_userId.empty() || i < 0 || i >= offlineCount()) return false;
    const auto& channels = evo_twitch::followed_channels().value;
    const int at = m_offline[static_cast<std::size_t>(i)];
    if (at < 0 || static_cast<std::size_t>(at) >= channels.size()) return false;
    const evo_twitch::ChannelCard& c = channels[static_cast<std::size_t>(at)];
    *login = c.login;
    *live = c.liveViewers >= 0;
    return true;
}

void TwitchFollowingScreen::move(int dx, int dy) {
    const int live_n = liveCount(), off_n = offlineCount();
    int next = -1;
    if (m_cursor < live_n) {
        next = twitch_screen::gridStep(m_cursor, live_n, kLiveCols, dx, dy);
        if (next < 0 && dy > 0 && off_n > 0) {
            /* Off the last live row: onto the avatar under the card. */
            next = live_n + std::min(off_n - 1, matchColumn(m_cursor % kLiveCols, kLiveCols, kOfflineCols));
        }
    } else {
        const int i = m_cursor - live_n;
        const int step = twitch_screen::gridStep(i, off_n, kOfflineCols, dx, dy);
        if (step >= 0) {
            next = live_n + step;
        } else if (dy < 0 && i < kOfflineCols && live_n > 0) {
            /* Off the first avatar row: onto the live card over it. */
            const int lastRow = liveRows(live_n) - 1;
            next = std::min(live_n - 1, lastRow * kLiveCols + matchColumn(i % kOfflineCols, kOfflineCols, kLiveCols));
        }
    }
    if (next < 0) {
        evo_feedback(EVO_FB_BOUNDARY);
        return;
    }
    m_cursor = next;
    evo_feedback(EVO_FB_MOVE);
}

bool TwitchFollowingScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released) {
    (void)held;
    (void)released;
    if (twitch_screen::fallbackActive()) return twitch_screen::fallbackInput(pressed);
    keepCursorValid();

    if (m_userId.empty()) {
        /* Nothing to move between: Cross logs in, Left opens the rail. */
        if (pressed & PadButtons::Cross) {
            if (evo_twitch::auth_view().state == AuthState::Unknown) {
                /* The saved login isn't read yet: it may well be there. */
                evo_feedback(EVO_FB_BOUNDARY);
            } else if (auto sm = Application::getInstance().getScreenManager()) {
                evo_feedback(EVO_FB_OPEN);
                sm->navigateTo(ScreenId::TwitchLogin);
            }
            return true;
        }
        if (pressed & PadButtons::Circle) {
            evo_feedback(EVO_FB_CANCEL);
            if (auto sm = Application::getInstance().getScreenManager()) sm->navigateBack(ScreenId::TwitchHome);
            return true;
        }
        if (pressed & PadButtons::Left) return false;  /* the ScreenManager opens the rail */
        if (pressed & (PadButtons::Up | PadButtons::Down | PadButtons::Right)) {
            evo_feedback(EVO_FB_BOUNDARY);
            return true;
        }
        return false;
    }

    if (pressed & PadButtons::Up) {
        move(0, -1);
        return true;
    }
    if (pressed & PadButtons::Down) {
        move(0, 1);
        return true;
    }
    if (pressed & PadButtons::Left) {
        const int live_n = liveCount();
        const int cols = m_cursor < live_n ? kLiveCols : kOfflineCols;
        const int at = m_cursor < live_n ? m_cursor : m_cursor - live_n;
        if (live_n + offlineCount() == 0 || at % cols == 0) return false;  /* the ScreenManager opens the rail */
        move(-1, 0);
        return true;
    }
    if (pressed & PadButtons::Right) {
        move(1, 0);
        return true;
    }
    const bool scopeMissing = evo_twitch::followed_scope_missing();
    if (pressed & PadButtons::Cross) {
        std::string login;
        bool live = false;
        if (!focused(&login, &live)) {
            if (scopeMissing) {
                /* "Log in again from Settings": that's where logging out is. */
                evo_feedback(EVO_FB_OPEN);
                if (auto sm = Application::getInstance().getScreenManager()) sm->navigateTo(ScreenId::Settings);
            } else {
                evo_feedback(EVO_FB_BOUNDARY);
            }
        } else if (live) {
            if (twitch_screen::play(login, m_startingLogin)) m_startingLogin = login;
        } else {
            evo_feedback(EVO_FB_OPEN);
            TwitchChannelScreen::show(login);
        }
        return true;
    }
    if (pressed & PadButtons::Square) {
        std::string login;
        bool live = false;
        if (!focused(&login, &live)) {
            evo_feedback(EVO_FB_BOUNDARY);
            return true;
        }
        evo_feedback(EVO_FB_OPEN);
        TwitchChannelScreen::show(login);
        return true;
    }
    if (pressed & PadButtons::Triangle) {
        if (scopeMissing) {
            evo_feedback(EVO_FB_BOUNDARY);  /* asking again can't help */
            return true;
        }
        evo_feedback(EVO_FB_CONFIRM);
        refresh(true);
        return true;
    }
    if (pressed & PadButtons::Circle) {
        evo_feedback(EVO_FB_CANCEL);
        if (auto sm = Application::getInstance().getScreenManager()) sm->navigateBack(ScreenId::TwitchHome);
        return true;
    }
    return false;
}

void TwitchFollowingScreen::render(uint32_t* framebuffer, int width, int height) {
    if (twitch_screen::fallbackActive()) {
        twitch_screen::renderFallback(framebuffer, width, height);
        return;
    }
    const auto& live = evo_twitch::followed_live();
    const auto& channels = evo_twitch::followed_channels();
    const AuthState auth = evo_twitch::auth_view().state;
    const bool in = !m_userId.empty();
    if (in) {
        /* One toast per outage: both lists fail for one reason. */
        m_liveToast.check(live, &m_outage);
        m_channelsToast.check(channels, &m_outage);
    }
    keepCursorValid();
    const int live_n = liveCount(), off_n = in ? offlineCount() : 0;
    if (!evo_twitch_launcher_starting()) m_startingLogin.clear();

    bool railFocused = false;
    if (auto sm = Application::getInstance().getScreenManager()) railFocused = sm->isRailFocused();

    static evo_hui_twitch_following_params_t p;
    std::memset(&p, 0, sizeof(p));
    p.login_state = in ? EVO_HUI_TWITCH_FOLLOWING_LOGGED_IN
                       : auth == AuthState::Unknown ? EVO_HUI_TWITCH_FOLLOWING_CHECKING
                                                    : EVO_HUI_TWITCH_FOLLOWING_LOGGED_OUT;
    p.live_status = in ? twitch_screen::status(live.status) : EVO_HUI_TWITCH_LOADING;
    p.live_error = live.error.c_str();
    p.channels_status = in ? twitch_screen::status(channels.status) : EVO_HUI_TWITCH_LOADING;
    p.channels_error = channels.error.c_str();
    p.scope_missing = in && evo_twitch::followed_scope_missing() ? 1 : 0;

    /* Pictures only for the rows on screen or close to them: the kit keeps
     * the cursor's row second from the top, so the one above it, its own and
     * the next few (avatar rows are short). Rows count through both grids. */
    const int lrows = liveRows(live_n);
    const int cursorRow = m_cursor < live_n ? m_cursor / kLiveCols : lrows + (m_cursor - live_n) / kOfflineCols;
    const int fromRow = std::max(0, cursorRow - 1), toRow = std::max(cursorRow, 1) + 3;
    p.live_count = live_n;
    for (int i = 0; i < live_n; ++i) {
        const int row = i / kLiveCols;
        twitch_screen::fill(p.live[i], live.value[static_cast<std::size_t>(i)],
                            row >= fromRow && row <= toRow ? &m_pictures : nullptr);
    }
    p.channel_count = off_n;
    for (int i = 0; i < off_n; ++i) {
        const int row = lrows + i / kOfflineCols;
        const evo_twitch::ChannelCard& c = channels.value[static_cast<std::size_t>(m_offline[static_cast<std::size_t>(i)])];
        twitch_screen::fill(p.channels[i], c, row >= fromRow && row <= toRow ? &m_pictures : nullptr);
    }
    m_pictures.endFrame();
    p.cursor = m_cursor;
    p.rail_focused = railFocused ? 1 : 0;
    p.starting_login = m_startingLogin.empty() ? nullptr : m_startingLogin.c_str();

#if defined(EVO_AGC_DEVICE)
    evo_hui_update_twitch_following(&p);
    evo_rmlui_render_kit_screen(evo_hui_render_twitch_following, width, height);
#else
    (void)width;
    (void)height;
#endif
}

} // namespace evo
