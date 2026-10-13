#include "evo/screens/AudioTrackPickerScreen.hpp"
#include "evo/Application.hpp"
#include "evo_rmlui_bridge.h"
#include "evo_feedback.h"
#include "evo_twitch.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

namespace evo {

AudioTrackPickerScreen::AudioTrackPickerScreen()
    : StatefulScreen("AudioTrackPickerScreen") {
}

void AudioTrackPickerScreen::onEnter() {
    StatefulScreen::onEnter();
    refreshRows();
    m_selectedIndex = m_currentIndex >= 0 ? m_currentIndex : 0;
    m_scrollOffset = 0;
    if (m_selectedIndex >= EVO_RMLUI_LIST_ROWS) {
        m_scrollOffset = m_selectedIndex - EVO_RMLUI_LIST_ROWS + 1;
    }
}

void AudioTrackPickerScreen::onExit() {
    StatefulScreen::onExit();
}

void AudioTrackPickerScreen::refreshRows() {
    m_rows.clear();
    m_currentIndex = -1;

    const evo_twitch_quality_t* qualities = nullptr;
    const int count = evo_twitch_qualities(&qualities);
    for (int i = 0; i < count; ++i) {
        const evo_twitch_quality_t& q = qualities[i];
        Row row;
        row.name = q.name;
        row.label = q.label;
        row.audioOnly = q.height <= 0;
        char detail[64];
        if (row.audioOnly)
            std::snprintf(detail, sizeof detail, "Just the sound");
        else if (q.fps > 0)
            std::snprintf(detail, sizeof detail, "%d x %d, %d fps", q.width, q.height, q.fps);
        else
            std::snprintf(detail, sizeof detail, "%d x %d", q.width, q.height);
        row.detail = detail;
        m_rows.push_back(std::move(row));
    }
    const int current = evo_twitch_current_quality();
    if (current >= 0 && current < static_cast<int>(m_rows.size())) m_currentIndex = current;
}

void AudioTrackPickerScreen::navigate(int delta) {
    if (m_rows.empty()) return;
    int n = static_cast<int>(m_rows.size());
    int next = m_selectedIndex + delta;
    if (next < 0)      { m_selectedIndex = 0;     evo_feedback(EVO_FB_BOUNDARY); return; }
    if (next >= n)     { m_selectedIndex = n - 1; evo_feedback(EVO_FB_BOUNDARY); return; }
    m_selectedIndex = next;

    if (m_selectedIndex >= m_scrollOffset + EVO_RMLUI_LIST_ROWS) {
        m_scrollOffset = m_selectedIndex - EVO_RMLUI_LIST_ROWS + 1;
    } else if (m_selectedIndex < m_scrollOffset) {
        m_scrollOffset = m_selectedIndex;
    }

    evo_feedback(EVO_FB_MOVE);
}

/* Back to the player, which is under the picker in the history. */
void AudioTrackPickerScreen::close() {
    if (auto sm = Application::getInstance().getScreenManager()) {
        if (!sm->navigateBack()) {
            sm->navigateTo(ScreenId::Player);
        }
    }
}

void AudioTrackPickerScreen::activateSelection() {
    if (m_selectedIndex < 0 || m_selectedIndex >= static_cast<int>(m_rows.size())) {
        close();
        return;
    }
    evo_feedback(EVO_FB_CONFIRM);
    /* The one playing: nothing to switch. Another: the launcher looks the
     * stream up again while this quality keeps playing, then restarts the
     * player in the new one; it toasts what happens either way. */
    if (m_selectedIndex != m_currentIndex)
        evo_twitch_switch_quality(m_rows[m_selectedIndex].name.c_str());
    close();
}

bool AudioTrackPickerScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released) {
    (void)held;
    (void)released;

    if (pressed & PadButtons::Up)    { navigate(-1); return true; }
    if (pressed & PadButtons::Down)  { navigate(1);  return true; }
    if (pressed & PadButtons::Cross) { activateSelection(); return true; }
    /* R2 opened it; R2 closes it too. */
    if (pressed & (PadButtons::Circle | PadButtons::R2)) {
        evo_feedback(EVO_FB_CANCEL);
        close();
        return true;
    }
    return false;
}

void AudioTrackPickerScreen::update(double deltaMs) {
    StatefulScreen::update(deltaMs);
}

void AudioTrackPickerScreen::render(uint32_t* framebuffer, int width, int height) {
    evo_rmlui_list_params_t params;
    std::memset(&params, 0, sizeof(params));

    params.title = "Quality";
    params.subtitle = "Lower it if the stream keeps buffering";
    params.section = -1;          /* not a rail destination */
    params.rail_focused = 0;

    int total = static_cast<int>(m_rows.size());
    params.total_count = total;
    params.cursor_index = total ? m_selectedIndex : -1;

    if (total == 0) {
        params.is_empty = 1;
        params.empty_title = "No other qualities";
        params.empty_hint = "This stream comes in one quality only.";
        params.empty_icon = "../icons/icon_tv.png";
    } else {
        int rowsToDisplay = std::min(EVO_RMLUI_LIST_ROWS, std::max(0, total - m_scrollOffset));
        params.row_count = rowsToDisplay;
        for (int i = 0; i < rowsToDisplay; ++i) {
            int idx = m_scrollOffset + i;
            params.rows[i].title = m_rows[idx].label.c_str();
            params.rows[i].detail = m_rows[idx].detail.c_str();
            params.rows[i].badge = "";
            params.rows[i].icon_path = m_rows[idx].audioOnly ? "../icons/icon_volume.png"
                                                             : "../icons/icon_tv.png";
            params.rows[i].progress = -1;
            params.rows[i].has_chevron = 0;
            params.rows[i].is_focused = (idx == m_selectedIndex);
            params.rows[i].is_checked = (idx == m_currentIndex);
        }
    }

    params.hint_count = 2;
    params.hints[0].glyph_path = "../icons/btn_cross.png";
    params.hints[0].label = "SELECT";
    params.hints[1].glyph_path = "../icons/btn_circle.png";
    params.hints[1].label = "BACK";

    evo_rmlui_update_list(&params);
    evo_rmlui_render_list(framebuffer, width, height);
}

} // namespace evo
