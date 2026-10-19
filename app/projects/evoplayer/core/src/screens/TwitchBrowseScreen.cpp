#include "evo/screens/TwitchBrowseScreen.hpp"
#include "evo/screens/TwitchCategoryScreen.hpp"
#include "evo/screens/TwitchScreenData.hpp"
#include "evo/Application.hpp"
#include "evo_rmlui_bridge.h"
#include "evo_twitch_settings.h"

#include <algorithm>
#include <cstring>

namespace evo {

namespace {
constexpr int kCols = EVO_HUI_TWITCH_BROWSE_COLS;
} // namespace

namespace twitch_screen {

void LanguageStepper::step(bool next) {
    const std::string from = shown();
    m_code = next ? evo_twitch::next_browse_language(from) : evo_twitch::prev_browse_language(from);
    m_pending = true;
    m_dueMs = evo_twitch::data_now_ms() + kDelayMs;
    evo_feedback(EVO_FB_MOVE);
    const std::string label = evo_twitch::browse_language_label(m_code);
    toast("Stream language", label.c_str());
}

std::string LanguageStepper::shown() const {
    return m_pending ? m_code : evo_twitch::settings().browseLanguage;
}

bool LanguageStepper::apply(bool force) {
    if (!m_pending || (!force && evo_twitch::data_now_ms() < m_dueMs)) return false;
    m_pending = false;
    if (m_code == evo_twitch::settings().browseLanguage) return false;
    evo_twitch::set_browse_language(m_code);
    evo_twitch::save_settings(evo_twitch::settings());
    evo_twitch::request_home_streams();
    return true;
}

std::string languageNote(const std::string& code) {
    if (code.empty()) return std::string();
    return evo_twitch::browse_language_label(code) + " streams";
}

} // namespace twitch_screen

TwitchBrowseScreen::TwitchBrowseScreen()
    : StatefulScreen("TwitchBrowseScreen") {
}

void TwitchBrowseScreen::onEnter() {
    StatefulScreen::onEnter();
    m_refreshToast.sync(evo_twitch::home_categories());
    refresh(false);
}

void TwitchBrowseScreen::onExit() {
    m_language.apply(true);
    StatefulScreen::onExit();
}

void TwitchBrowseScreen::update(double deltaMs) {
    StatefulScreen::update(deltaMs);
    m_language.apply(false);
}

void TwitchBrowseScreen::refresh(bool force) {
    if (force || twitch_screen::due(evo_twitch::home_categories(), evo_twitch::data_now_ms()))
        evo_twitch::request_home_categories();
}

int TwitchBrowseScreen::count() const {
    return std::min<int>(static_cast<int>(evo_twitch::home_categories().value.size()),
                         EVO_HUI_TWITCH_BROWSE_CATEGORIES);
}

void TwitchBrowseScreen::move(int dx, int dy) {
    const int next = twitch_screen::gridStep(m_cursor, count(), kCols, dx, dy);
    if (next < 0) {
        evo_feedback(EVO_FB_BOUNDARY);
        return;
    }
    m_cursor = next;
    evo_feedback(EVO_FB_MOVE);
}

void TwitchBrowseScreen::activate() {
    if (m_cursor >= count()) {
        evo_feedback(EVO_FB_ERROR);
        return;
    }
    const evo_twitch::CategoryCard& c = evo_twitch::home_categories().value[static_cast<std::size_t>(m_cursor)];
    evo_feedback(EVO_FB_OPEN);
    TwitchCategoryScreen::show(c.name, c.viewers, c.boxArtUrl);
}

bool TwitchBrowseScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released) {
    (void)held;
    (void)released;
    if (twitch_screen::fallbackActive()) return twitch_screen::fallbackInput(pressed);

    if (pressed & PadButtons::Up) {
        move(0, -1);
        return true;
    }
    if (pressed & PadButtons::Down) {
        move(0, 1);
        return true;
    }
    if (pressed & PadButtons::Left) {
        if (count() == 0 || m_cursor % kCols == 0) return false;  /* the ScreenManager opens the rail */
        move(-1, 0);
        return true;
    }
    if (pressed & PadButtons::Right) {
        move(1, 0);
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
    /* L1 and R1 step the stream language (see the class comment). */
    if (pressed & (PadButtons::L1 | PadButtons::R1)) {
        m_language.step((pressed & PadButtons::R1) != 0);
        return true;
    }
    if (pressed & PadButtons::Circle) {
        evo_feedback(EVO_FB_CANCEL);
        if (auto sm = Application::getInstance().getScreenManager()) sm->navigateBack(ScreenId::TwitchHome);
        return true;
    }
    return false;
}

void TwitchBrowseScreen::render(uint32_t* framebuffer, int width, int height) {
    if (twitch_screen::fallbackActive()) {
        twitch_screen::renderFallback(framebuffer, width, height);
        return;
    }
    const auto& categories = evo_twitch::home_categories();
    m_refreshToast.check(categories);
    const int n = count();
    /* A refresh can shorten the list under the cursor. */
    m_cursor = std::clamp(m_cursor, 0, std::max(0, n - 1));

    bool railFocused = false;
    if (auto sm = Application::getInstance().getScreenManager()) railFocused = sm->isRailFocused();

    static evo_hui_twitch_browse_params_t p;
    std::memset(&p, 0, sizeof(p));
    p.status = twitch_screen::status(categories.status);
    p.error = categories.error.c_str();
    p.category_count = n;
    /* Box art for the rows on screen: the kit keeps the cursor's row second
     * from the top, so the one above it, its own and the two below. */
    const int row = m_cursor / kCols;
    const int from = std::max(0, (row - 1) * kCols), to = std::min(n, (std::max(row, 1) + 2) * kCols);
    for (int i = 0; i < n; ++i)
        twitch_screen::fill(p.categories[i], categories.value[static_cast<std::size_t>(i)],
                            i >= from && i < to ? &m_pictures : nullptr);
    m_pictures.endFrame();
    p.cursor = m_cursor;
    p.rail_focused = railFocused ? 1 : 0;
    /* The categories are everyone's; the language is the streams' in them. */
    m_languageLine = twitch_screen::languageNote(m_language.shown());
    p.language = m_languageLine.c_str();

#if defined(EVO_AGC_DEVICE)
    evo_hui_update_twitch_browse(&p);
    evo_rmlui_render_kit_screen(evo_hui_render_twitch_browse, width, height);
#else
    (void)width;
    (void)height;
#endif
}

} // namespace evo
