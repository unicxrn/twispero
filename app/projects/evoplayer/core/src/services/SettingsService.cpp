#include "evo/services/SettingsService.hpp"
#include "evo_boot_log.h"
#include "evo_data_path.h"
#include "evo_jailbreak.h"
#include "evo_twitch_settings.h"
#include "evo_theme.h"
#include "evo_toast.h"
#include "evo_feedback.h"
#include "evo_keyboard.h"
#include "evo_rmlui_bridge.h"

#include <cstdio>

namespace evo {

/*
 * The settings live in the app's one settings file, twitch_settings.json,
 * behind the evo_twitch settings API (no JSON code here). The setters change
 * them for this run and apply them; saveSettings() writes them.
 */
bool SettingsService::isSoundFeedbackEnabled() const {
    return evo_twitch::settings().navigationSounds;
}

void SettingsService::setSoundFeedbackEnabled(bool enabled) {
    evo_twitch::set_navigation_sounds(enabled);
    evo_feedback_set_sound(enabled ? 1 : 0);
}

bool SettingsService::isLightbarFeedbackEnabled() const {
    return evo_twitch::settings().lightbar;
}

void SettingsService::setLightbarFeedbackEnabled(bool enabled) {
    evo_twitch::set_lightbar(enabled);
    evo_feedback_set_lightbar(enabled ? 1 : 0);
}

int SettingsService::getKeyboardType() const {
    return evo_twitch::settings().keyboard == twitch::Keyboard::OnScreen ? EVO_KEYBOARD_TYPE_VIRTUAL
                                                                         : EVO_KEYBOARD_TYPE_NATIVE;
}

void SettingsService::setKeyboardType(int type) {
    evo_twitch::set_keyboard(type == EVO_KEYBOARD_TYPE_VIRTUAL ? twitch::Keyboard::OnScreen
                                                               : twitch::Keyboard::System);
    evo_keyboard_set_type(getKeyboardType());
}

bool SettingsService::isDebugOverlayEnabled() const {
    return evo_twitch::settings().debugOverlay;
}

void SettingsService::setDebugOverlayEnabled(bool enabled) {
    evo_twitch::set_debug_overlay(enabled);
}

/* False before the sandbox opens, when the file can't be written yet; that is
 * expected and says nothing. */
bool SettingsService::saveSettings() {
    if (evo_twitch::save_settings(evo_twitch::settings())) return true;
    if (evo_jailbreak_is_open()) toast_error("Couldn't save the settings", nullptr);
    return false;
}

/*
 * Applies the saved settings: the keyboard, the navigation sounds and the
 * controller light. Called at boot and again after a late unjail, when the
 * settings file can first be read (until then evo_twitch::settings() gives
 * the defaults).
 */
bool SettingsService::loadSettings() {
    const twitch::AppSettings& s = evo_twitch::settings();
    evo_keyboard_set_type(getKeyboardType());
    evo_feedback_set_sound(s.navigationSounds ? 1 : 0);
    evo_feedback_set_lightbar(s.lightbar ? 1 : 0);

    /* EVO kept its settings in evo_player_settings.cfg. Nothing reads it any
     * more, so it goes, once, as soon as the data folder is ours. */
    if (!m_oldFileRemoved && evo_jailbreak_is_open()) {
        m_oldFileRemoved = true;
        if (std::remove(evo_data_path("evo_player_settings.cfg")) == 0)
            evo_boot_log("settings: removed the old evo_player_settings.cfg");
    }

    /* The app's colours (always the TWITCH theme) into the RmlUi screens, and
     * the controller light to match. */
    syncThemeToRmlUi();
    evo_feedback_refresh_lightbar();
    return true;
}

void SettingsService::syncThemeToRmlUi() {
    const evo_theme* t = evo_theme_current();
    if (!t) return;

    evo_rmlui_theme_t rmlUiTheme;
    rmlUiTheme.name = t->name;
    rmlUiTheme.bg_top = t->bg_top;
    rmlUiTheme.bg_bottom = t->bg_bottom;
    rmlUiTheme.surface = t->surface;
    rmlUiTheme.surface_sel = t->surface_sel;
    rmlUiTheme.border = t->border;
    rmlUiTheme.border_sel = t->border_sel;
    rmlUiTheme.accent = t->accent;
    rmlUiTheme.accent_soft = t->accent_soft;
    rmlUiTheme.accent_alt = t->accent_alt;
    rmlUiTheme.text_primary = t->text_primary;
    rmlUiTheme.text_secondary = t->text_secondary;
    rmlUiTheme.text_muted = t->text_muted;

    evo_rmlui_set_theme(&rmlUiTheme);
}

} // namespace evo
