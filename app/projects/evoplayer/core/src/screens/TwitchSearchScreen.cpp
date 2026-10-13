#include "evo/screens/TwitchSearchScreen.hpp"
#include "evo/screens/TwitchCategoryScreen.hpp"
#include "evo/screens/TwitchChannelScreen.hpp"
#include "evo/screens/TwitchScreenData.hpp"
#include "evo/Application.hpp"
#include "evo_keyboard.h"
#include "evo_rmlui_bridge.h"

#include <algorithm>
#include <cstring>

namespace evo {

namespace {

/* Longer than anyone types with a pad; Twitch's own field takes about this. */
constexpr int kMaxQuery = 60;

std::string trimmed(const char* text) {
    std::string s = text ? text : "";
    const std::size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    const std::size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

} // namespace

TwitchSearchScreen::TwitchSearchScreen()
    : StatefulScreen("TwitchSearchScreen") {
    /* The kit reads m_row as the params' focus_row. */
    static_assert(Field == EVO_HUI_TWITCH_SEARCH_FIELD, "Row must match EVO_HUI_TWITCH_SEARCH_*");
    static_assert(Channels == EVO_HUI_TWITCH_SEARCH_CHANNEL, "Row must match EVO_HUI_TWITCH_SEARCH_*");
    static_assert(Categories == EVO_HUI_TWITCH_SEARCH_CATEGORY, "Row must match EVO_HUI_TWITCH_SEARCH_*");
    static_assert(Recent == EVO_HUI_TWITCH_SEARCH_RECENT, "Row must match EVO_HUI_TWITCH_SEARCH_*");
}

void TwitchSearchScreen::onEnter() {
    StatefulScreen::onEnter();
    m_channelsToast.sync(evo_twitch::search_channels());
    m_categoriesToast.sync(evo_twitch::search_categories());
    refresh(false);
}

void TwitchSearchScreen::OnQuerySubmitted(const char* text, void* userdata) {
    auto* self = static_cast<TwitchSearchScreen*>(userdata);
    if (self) self->submit(trimmed(text));
}

void TwitchSearchScreen::submit(const std::string& query) {
    const bool same = query == m_query;
    m_query = query;
    m_row = Field;
    if (!same) {
        m_channelCol = 0;
        m_categoryCol = 0;
    }
    /* The newest of the recent searches (a blank one is left out). */
    evo_twitch::history_record_search(query);
    m_recentCursor = 0;
    /* The same words again search again. */
    refresh(same);
}

bool TwitchSearchScreen::current() const {
    return !m_query.empty() && m_requested == m_query;
}

void TwitchSearchScreen::refresh(bool force) {
    if (m_query.empty()) return;
    const unsigned long long now = evo_twitch::data_now_ms();
    if (force || m_requested != m_query || twitch_screen::due(evo_twitch::search_channels(), now) ||
        twitch_screen::due(evo_twitch::search_categories(), now)) {
        evo_twitch::request_search(m_query);
        m_requested = m_query;
    }
}

int TwitchSearchScreen::itemCount(int row) const {
    switch (row) {
        case Field:      return 1;
        case Channels:   return current() ? std::min<int>(static_cast<int>(evo_twitch::search_channels().value.size()),
                                                          EVO_HUI_TWITCH_SEARCH_CHANNELS)
                                          : 0;
        case Categories: return current() ? std::min<int>(static_cast<int>(evo_twitch::search_categories().value.size()),
                                                          EVO_HUI_TWITCH_SEARCH_CATEGORIES)
                                          : 0;
        /* Only while there is no query: then they stand under the field. */
        case Recent:     return m_query.empty() ? std::min<int>(static_cast<int>(evo_twitch::history_searches().size()),
                                                                EVO_HUI_TWITCH_SEARCH_RECENTS)
                                                : 0;
        default:         return 0;
    }
}

void TwitchSearchScreen::keepRowValid() {
    /* A refresh can empty the row under the cursor: the nearest row above it
     * that has something (the field always does). */
    while (m_row > Field && itemCount(m_row) == 0) --m_row;
}

void TwitchSearchScreen::moveRow(int dir) {
    /* Field, Channels, Categories, Recent: the recent searches only show
     * without a query and the results only with one, so Down from the field
     * reaches whichever is there. */
    for (int row = m_row + dir; row >= Field && row <= Recent; row += dir) {
        if (itemCount(row) > 0) {
            m_row = row;
            evo_feedback(EVO_FB_MOVE);
            return;
        }
    }
    evo_feedback(EVO_FB_BOUNDARY);
}

void TwitchSearchScreen::moveCol(int dir) {
    int* col = m_row == Channels ? &m_channelCol : (m_row == Categories ? &m_categoryCol : nullptr);
    const int count = itemCount(m_row);
    const int next = col ? *col + dir : -1;
    if (!col || next < 0 || next >= count) {
        evo_feedback(EVO_FB_BOUNDARY);
        return;
    }
    *col = next;
    evo_feedback(EVO_FB_MOVE);
}

void TwitchSearchScreen::activate() {
    if (m_row == Field) {
        evo_feedback(EVO_FB_OPEN);
        evo_keyboard_open("Search Twitch", m_query.c_str(), kMaxQuery, &TwitchSearchScreen::OnQuerySubmitted, this);
        return;
    }
    if (m_row == Channels && m_channelCol < itemCount(Channels)) {
        const evo_twitch::ChannelCard& c = evo_twitch::search_channels().value[static_cast<std::size_t>(m_channelCol)];
        evo_feedback(EVO_FB_OPEN);
        TwitchChannelScreen::show(c.login);
        return;
    }
    if (m_row == Categories && m_categoryCol < itemCount(Categories)) {
        const evo_twitch::CategoryCard& c =
            evo_twitch::search_categories().value[static_cast<std::size_t>(m_categoryCol)];
        evo_feedback(EVO_FB_OPEN);
        TwitchCategoryScreen::show(c.name, c.viewers, twitch_screen::boxArtUrl(c.boxArtUrl));
        return;
    }
    if (m_row == Recent && m_recentCursor < itemCount(Recent)) {
        /* Copied: searching moves it to the top of the list. */
        const std::string query = evo_twitch::history_searches()[static_cast<std::size_t>(m_recentCursor)];
        evo_feedback(EVO_FB_CONFIRM);
        submit(query);
        return;
    }
    evo_feedback(EVO_FB_ERROR);
}

bool TwitchSearchScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released) {
    (void)held;
    (void)released;
    if (twitch_screen::fallbackActive()) return twitch_screen::fallbackInput(pressed);

    /* The recent searches are a list: Up and Down go through it first. */
    if (m_row == Recent && (pressed & PadButtons::Up) && m_recentCursor > 0) {
        --m_recentCursor;
        evo_feedback(EVO_FB_MOVE);
        return true;
    }
    if (m_row == Recent && (pressed & PadButtons::Down)) {
        if (m_recentCursor + 1 < itemCount(Recent)) {
            ++m_recentCursor;
            evo_feedback(EVO_FB_MOVE);
        } else {
            evo_feedback(EVO_FB_BOUNDARY);
        }
        return true;
    }
    if (pressed & PadButtons::Up) {
        moveRow(-1);
        return true;
    }
    if (pressed & PadButtons::Down) {
        moveRow(1);
        return true;
    }
    if (pressed & PadButtons::Left) {
        const int col = m_row == Channels ? m_channelCol : (m_row == Categories ? m_categoryCol : 0);
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
    if ((pressed & PadButtons::Square) && m_row == Recent) {
        if (m_recentCursor >= itemCount(Recent)) {
            evo_feedback(EVO_FB_BOUNDARY);
            return true;
        }
        evo_feedback(EVO_FB_TOGGLE);
        /* Copied: the list changes under the reference. */
        const std::string query = evo_twitch::history_searches()[static_cast<std::size_t>(m_recentCursor)];
        evo_twitch::history_remove_search(query);
        /* The next one takes its place; the last one gone, back to the field. */
        m_recentCursor = std::min(m_recentCursor, std::max(0, itemCount(Recent) - 1));
        if (itemCount(Recent) == 0) m_row = Field;
        return true;
    }
    if (pressed & PadButtons::Square) {
        if (m_row != Field || m_query.empty()) {
            evo_feedback(EVO_FB_BOUNDARY);
            return true;
        }
        evo_feedback(EVO_FB_TOGGLE);
        m_query.clear();
        m_channelCol = 0;
        m_categoryCol = 0;
        return true;
    }
    if (pressed & PadButtons::Triangle) {
        if (m_query.empty()) {
            evo_feedback(EVO_FB_BOUNDARY);
            return true;
        }
        evo_feedback(EVO_FB_CONFIRM);
        refresh(true);
        return true;
    }
    if (pressed & PadButtons::Circle) {
        evo_feedback(EVO_FB_CANCEL);
        if (m_row != Field) {
            m_row = Field;
            return true;
        }
        if (auto sm = Application::getInstance().getScreenManager()) sm->navigateBack(ScreenId::TwitchHome);
        return true;
    }
    return false;
}

void TwitchSearchScreen::render(uint32_t* framebuffer, int width, int height) {
    if (twitch_screen::fallbackActive()) {
        twitch_screen::renderFallback(framebuffer, width, height);
        return;
    }
    const auto& channels = evo_twitch::search_channels();
    const auto& categories = evo_twitch::search_categories();
    if (current()) {
        /* One toast for a failed refresh: both halves fail for one reason. */
        if (m_channelsToast.check(channels))
            m_categoriesToast.sync(categories);
        else
            m_categoriesToast.check(categories);
    }

    /* A refresh can shorten a row under the cursor. */
    const int chn = itemCount(Channels), cn = itemCount(Categories), rn = itemCount(Recent);
    m_channelCol = std::clamp(m_channelCol, 0, std::max(0, chn - 1));
    m_categoryCol = std::clamp(m_categoryCol, 0, std::max(0, cn - 1));
    m_recentCursor = std::clamp(m_recentCursor, 0, std::max(0, rn - 1));
    keepRowValid();

    bool railFocused = false;
    if (auto sm = Application::getInstance().getScreenManager()) railFocused = sm->isRailFocused();

    static evo_hui_twitch_search_params_t p;
    std::memset(&p, 0, sizeof(p));
    p.query = m_query.c_str();
    p.channels_status = current() ? twitch_screen::status(channels.status) : EVO_HUI_TWITCH_LOADING;
    p.categories_status = current() ? twitch_screen::status(categories.status) : EVO_HUI_TWITCH_LOADING;
    p.channels_error = channels.error.c_str();
    p.categories_error = categories.error.c_str();
    /* Ten of each at most: every picture is asked for (they are small). */
    p.channel_count = chn;
    for (int i = 0; i < chn; ++i)
        twitch_screen::fill(p.channels[i], channels.value[static_cast<std::size_t>(i)], &m_pictures);
    p.category_count = cn;
    for (int i = 0; i < cn; ++i)
        twitch_screen::fill(p.categories[i], categories.value[static_cast<std::size_t>(i)], &m_pictures);
    m_pictures.endFrame();

    /* The rail keeps the row: the page must not move when it opens. */
    p.focus_row = m_row;
    p.channel_col = m_channelCol;
    p.category_col = m_categoryCol;
    p.rail_focused = railFocused ? 1 : 0;
    const std::vector<std::string>& recents = evo_twitch::history_searches();
    p.recent_count = rn;
    for (int i = 0; i < rn; ++i) p.recents[i] = recents[static_cast<std::size_t>(i)].c_str();
    p.recent_cursor = m_recentCursor;

#if defined(EVO_AGC_DEVICE)
    evo_hui_update_twitch_search(&p);
    evo_rmlui_render_kit_screen(evo_hui_render_twitch_search, width, height);
#else
    (void)width;
    (void)height;
#endif
}

} // namespace evo
