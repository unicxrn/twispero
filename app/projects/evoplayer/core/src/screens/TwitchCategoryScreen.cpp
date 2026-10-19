#include "evo/screens/TwitchCategoryScreen.hpp"
#include "evo/screens/TwitchChannelScreen.hpp"
#include "evo/screens/TwitchScreenData.hpp"
#include "evo/Application.hpp"
#include "evo_rmlui_bridge.h"
#include "evo_twitch_settings.h"

#include <algorithm>
#include <cstring>

namespace evo {

namespace {
constexpr int kCols = EVO_HUI_TWITCH_CATEGORY_COLS;
static_assert(EVO_HUI_TWITCH_CATEGORY_COLS == EVO_HUI_TWITCH_VIDEO_COLS, "both tabs move on one grid");
} // namespace

TwitchCategoryScreen::TwitchCategoryScreen()
    : StatefulScreen("TwitchCategoryScreen") {
}

void TwitchCategoryScreen::show(const std::string& name, int viewers, const std::string& boxArtUrl) {
    ScreenManager* sm = Application::getInstance().getScreenManager();
    if (!sm) return;
    auto* screen = dynamic_cast<TwitchCategoryScreen*>(sm->getScreen(ScreenId::TwitchCategory));
    if (!screen) return;
    screen->open(name, viewers, boxArtUrl);
    sm->navigateTo(ScreenId::TwitchCategory);
}

void TwitchCategoryScreen::open(const std::string& name, int viewers, const std::string& boxArtUrl) {
    m_name = name;
    m_viewers = viewers;
    m_boxArtUrl = boxArtUrl;
    m_cursor = 0;
    m_tab = LiveTab;
    m_focus = Grid;
    m_clipFilter = m_filterCursor = EVO_HUI_TWITCH_CLIPS_WEEK;
    m_clipCursor = 0;
}

void TwitchCategoryScreen::onEnter() {
    StatefulScreen::onEnter();
    m_refreshToast.sync(evo_twitch::category_streams());
    m_clipsToast.sync(evo_twitch::category_clips());
    refresh(false);
    if (m_tab == ClipsTab) refreshClips(false);
}

void TwitchCategoryScreen::onExit() {
    m_language.apply(true);
    StatefulScreen::onExit();
}

void TwitchCategoryScreen::update(double deltaMs) {
    StatefulScreen::update(deltaMs);
    /* L1/R1's language, once the presses stop: the live list again in it. */
    if (m_language.apply(false)) refresh(false);
}

std::string TwitchCategoryScreen::liveKey() const {
    return m_name + "\n" + evo_twitch::settings().browseLanguage;
}

void TwitchCategoryScreen::refresh(bool force) {
    if (m_name.empty()) return;
    if (force || m_requested != liveKey() ||
        twitch_screen::due(evo_twitch::category_streams(), evo_twitch::data_now_ms())) {
        evo_twitch::request_category_streams(m_name);
        m_requested = liveKey();
    }
}

std::string TwitchCategoryScreen::clipsKey() const {
    return std::to_string(m_clipFilter) + ":" + m_name;
}

bool TwitchCategoryScreen::clipsOurs() const {
    return !m_name.empty() && m_clipsRequested == clipsKey();
}

void TwitchCategoryScreen::refreshClips(bool force) {
    if (m_name.empty()) return;
    if (force || !clipsOurs() ||
        twitch_screen::due(evo_twitch::category_clips(), evo_twitch::data_now_ms(), twitch_screen::kVideosStaleMs)) {
        evo_twitch::request_category_clips(m_name, twitch_screen::clipPeriod(m_clipFilter));
        m_clipsRequested = clipsKey();
    }
}

int TwitchCategoryScreen::count() const {
    if (m_name.empty() || m_requested != liveKey()) return 0;
    return std::min<int>(static_cast<int>(evo_twitch::category_streams().value.size()),
                         EVO_HUI_TWITCH_CATEGORY_STREAMS);
}

int TwitchCategoryScreen::clipCount() const {
    if (!clipsOurs()) return 0;
    return std::min<int>(static_cast<int>(evo_twitch::category_clips().value.cards.size()),
                         EVO_HUI_TWITCH_CATEGORY_CLIPS);
}

bool TwitchCategoryScreen::tabLoading() const {
    if (m_tab == ClipsTab)
        return !clipsOurs() || twitch_screen::status(evo_twitch::category_clips().status) == EVO_HUI_TWITCH_LOADING;
    return m_requested != liveKey() ||
           twitch_screen::status(evo_twitch::category_streams().status) == EVO_HUI_TWITCH_LOADING;
}

std::string TwitchCategoryScreen::focusedLogin() const {
    if (m_tab == ClipsTab) {
        if (m_clipCursor >= clipCount()) return std::string();
        return evo_twitch::category_clips().value.cards[static_cast<std::size_t>(m_clipCursor)].channelLogin;
    }
    if (m_cursor >= count()) return std::string();
    return evo_twitch::category_streams().value[static_cast<std::size_t>(m_cursor)].login;
}

void TwitchCategoryScreen::keepFocusValid() {
    /* A refresh can shorten a list under its cursor. */
    m_cursor = std::clamp(m_cursor, 0, std::max(0, count() - 1));
    m_clipCursor = std::clamp(m_clipCursor, 0, std::max(0, clipCount() - 1));
    /* A list that came back empty or failed: the cursor goes where it can do
     * something (the chips or the tabs). One still loading keeps it, so the
     * first card has it once the list lands. */
    if (m_focus == Grid && tabCount() == 0 && !tabLoading()) {
        m_focus = m_tab == ClipsTab ? Filters : Tabs;
        m_filterCursor = m_clipFilter;
    }
}

void TwitchCategoryScreen::move(int dx, int dy) {
    int& cursor = tabCursor();
    const int next = twitch_screen::gridStep(cursor, tabCount(), kCols, dx, dy);
    if (next < 0) {
        evo_feedback(EVO_FB_BOUNDARY);
        return;
    }
    cursor = next;
    evo_feedback(EVO_FB_MOVE);
}

bool TwitchCategoryScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released) {
    (void)held;
    (void)released;
    if (twitch_screen::fallbackActive()) return twitch_screen::fallbackInput(pressed);
    keepFocusValid();
    const int n = tabCount();

    if (pressed & PadButtons::Up) {
        if (m_focus == Grid && n > 0 && tabCursor() >= kCols) {
            move(0, -1);
        } else if (m_focus == Grid) {
            m_focus = m_tab == ClipsTab ? Filters : Tabs;
            m_filterCursor = m_clipFilter;
            evo_feedback(EVO_FB_MOVE);
        } else if (m_focus == Filters) {
            m_focus = Tabs;
            evo_feedback(EVO_FB_MOVE);
        } else {
            evo_feedback(EVO_FB_BOUNDARY);
        }
        return true;
    }
    if (pressed & PadButtons::Down) {
        if (m_focus == Grid) {
            move(0, 1);
        } else if (m_focus == Tabs && m_tab == ClipsTab) {
            m_focus = Filters;
            m_filterCursor = m_clipFilter;
            evo_feedback(EVO_FB_MOVE);
        } else if (n > 0) {
            m_focus = Grid;
            evo_feedback(EVO_FB_MOVE);
        } else {
            evo_feedback(EVO_FB_BOUNDARY);
        }
        return true;
    }
    if (pressed & PadButtons::Left) {
        if (m_focus == Tabs) {
            if (m_tab == LiveTab) return false;  /* the ScreenManager opens the rail */
            m_tab = LiveTab;
            evo_feedback(EVO_FB_MOVE);
            refresh(false);
            return true;
        }
        if (m_focus == Filters) {
            if (m_filterCursor == 0) return false;
            --m_filterCursor;
            evo_feedback(EVO_FB_MOVE);
            return true;
        }
        if (n == 0 || tabCursor() % kCols == 0) return false;
        move(-1, 0);
        return true;
    }
    if (pressed & PadButtons::Right) {
        if (m_focus == Tabs) {
            if (m_tab == ClipsTab) {
                evo_feedback(EVO_FB_BOUNDARY);
                return true;
            }
            m_tab = ClipsTab;
            evo_feedback(EVO_FB_MOVE);
            refreshClips(false);
            return true;
        }
        if (m_focus == Filters) {
            if (m_filterCursor + 1 >= EVO_HUI_TWITCH_CLIP_FILTERS) {
                evo_feedback(EVO_FB_BOUNDARY);
                return true;
            }
            ++m_filterCursor;
            evo_feedback(EVO_FB_MOVE);
            return true;
        }
        move(1, 0);
        return true;
    }
    if (pressed & PadButtons::Cross) {
        if (m_focus == Filters) {
            if (m_filterCursor == m_clipFilter) {
                evo_feedback(EVO_FB_BOUNDARY);
                return true;
            }
            m_clipFilter = m_filterCursor;
            m_clipCursor = 0;
            evo_feedback(EVO_FB_CONFIRM);
            refreshClips(false);
            return true;
        }
        if (m_focus != Grid || n == 0) {
            evo_feedback(EVO_FB_BOUNDARY);
            return true;
        }
        if (m_tab == ClipsTab) {
            const evo_twitch::ClipCard& c = evo_twitch::category_clips().value.cards[static_cast<std::size_t>(m_clipCursor)];
            const std::string noAvatar;  /* a category's clips don't say whose picture */
            evo_twitch_video_info_t info = twitch_screen::clipInfo(c, noAvatar);
            if (c.game.empty()) info.category = m_name.c_str();
            if (twitch_screen::playVideo(c.slug, info, m_startingId)) m_startingId = c.slug;
            return true;
        }
        const std::string login = focusedLogin();
        if (twitch_screen::play(login, m_startingLogin)) m_startingLogin = login;
        return true;
    }
    if (pressed & PadButtons::Square) {
        const std::string login = m_focus == Grid ? focusedLogin() : std::string();
        if (login.empty()) {
            evo_feedback(EVO_FB_BOUNDARY);
            return true;
        }
        evo_feedback(EVO_FB_OPEN);
        TwitchChannelScreen::show(login);
        return true;
    }
    /* L1 and R1 step the stream language of the live list (and Home's, see
     * TwitchBrowseScreen). Clips are in every language. */
    if ((pressed & (PadButtons::L1 | PadButtons::R1)) && m_tab == LiveTab) {
        m_language.step((pressed & PadButtons::R1) != 0);
        return true;
    }
    if (pressed & PadButtons::Triangle) {
        evo_feedback(EVO_FB_CONFIRM);
        if (m_tab == ClipsTab)
            refreshClips(true);
        else
            refresh(true);
        return true;
    }
    if (pressed & PadButtons::Circle) {
        evo_feedback(EVO_FB_CANCEL);
        if (auto sm = Application::getInstance().getScreenManager()) sm->navigateBack(ScreenId::TwitchBrowse);
        return true;
    }
    return false;
}

void TwitchCategoryScreen::render(uint32_t* framebuffer, int width, int height) {
    if (twitch_screen::fallbackActive()) {
        twitch_screen::renderFallback(framebuffer, width, height);
        return;
    }
    const auto& streams = evo_twitch::category_streams();
    const auto& clips = evo_twitch::category_clips();
    if (m_requested == liveKey()) m_refreshToast.check(streams);
    if (clipsOurs()) m_clipsToast.check(clips);
    keepFocusValid();
    const int n = count();
    if (!evo_twitch_launcher_starting()) {
        m_startingLogin.clear();
        m_startingId.clear();
    }

    bool railFocused = false;
    if (auto sm = Application::getInstance().getScreenManager()) railFocused = sm->isRailFocused();

    static evo_hui_twitch_category_params_t p;
    std::memset(&p, 0, sizeof(p));
    p.name = m_name.c_str();
    p.viewers = m_viewers;
    p.art = twitch_screen::picture(m_boxArtUrl, twitch_screen::kBoxW, twitch_screen::kBoxH);
    if (m_name.empty()) {
        /* Only reachable by jumping here with the dev remote. */
        p.status = EVO_HUI_TWITCH_ERROR;
        p.error = "Pick a category in Browse";
    } else {
        p.status = m_requested == liveKey() ? twitch_screen::status(streams.status) : EVO_HUI_TWITCH_LOADING;
        /* As on a channel page: "Not found" alone says too little. */
        p.error = streams.error == "Not found" ? "Category not found" : streams.error.c_str();
    }
    p.tab = m_tab;
    p.focus = m_focus;
    p.filter = m_clipFilter;
    p.filter_cursor = m_filterCursor;

    /* Thumbnails for the open tab's rows on screen: the kit keeps the
     * cursor's row second from the top, so the one above it, its own and the
     * two below. */
    const int cursor = m_tab == ClipsTab ? m_clipCursor : m_cursor;
    const int row = m_focus == Grid ? cursor / kCols : 0;
    const int from = std::max(0, (row - 1) * kCols), to = (std::max(row, 1) + 2) * kCols;
    p.stream_count = n;
    for (int i = 0; i < n; ++i) {
        evo_hui_twitch_stream_t& o = p.streams[i];
        const bool shown = m_tab == LiveTab && i >= from && i < to;
        twitch_screen::fill(o, streams.value[static_cast<std::size_t>(i)], shown ? &m_pictures : nullptr);
        o.game = "";  /* every card here is in this category: the name says enough */
    }
    p.cursor = m_cursor;

    p.clips_status = EVO_HUI_TWITCH_LOADING;
    if (m_name.empty()) {
        p.clips_status = EVO_HUI_TWITCH_ERROR;
        p.clips_error = p.error;
    } else if (clipsOurs()) {
        const int cn = clipCount();
        p.clips_status = twitch_screen::status(clips.status);
        p.clips_error = clips.error == "Not found" ? "Category not found" : clips.error.c_str();
        p.clip_count = cn;
        for (int i = 0; i < cn; ++i) {
            const bool shown = m_tab == ClipsTab && i >= from && i < to;
            twitch_screen::fill(p.clips[i], clips.value.cards[static_cast<std::size_t>(i)], true,
                                shown ? &m_pictures : nullptr);
        }
    }
    p.clip_cursor = m_clipCursor;
    m_pictures.endFrame();
    p.rail_focused = railFocused ? 1 : 0;
    p.starting_login = m_startingLogin.empty() ? nullptr : m_startingLogin.c_str();
    p.starting_id = m_startingId.empty() ? nullptr : m_startingId.c_str();
    m_languageLine = twitch_screen::languageNote(m_language.shown());
    p.language = m_languageLine.c_str();

#if defined(EVO_AGC_DEVICE)
    evo_hui_update_twitch_category(&p);
    evo_rmlui_render_kit_screen(evo_hui_render_twitch_category, width, height);
#else
    (void)width;
    (void)height;
#endif
}

} // namespace evo
