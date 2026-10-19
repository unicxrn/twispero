#include "evo/screens/SettingsScreen.hpp"
#include "evo/Application.hpp"
#include "evo_rmlui_bridge.h"
#include "evo_feedback.h"
#include "evo_boot_log.h"
#include "evo_keyboard.h"
#include "evo_twitch_auth.h"
#include "evo_twitch_history.h"
#include "evo_twitch_progress.h"
#include "evo_twitch_settings.h"
#include "evo_toast.h"
#include "twitch/app.hpp"

#include <cstddef>
#include <cstdio>
#include <cstring>

namespace evo {

namespace {

constexpr int kMaxDefs    = 8;
constexpr int kMaxOptions = twitch::kQualityCount;   /* Default quality is the longest list */

/*
 * Every setting the screen can show, by name rather than by position (#118).
 * Toggling, picking a choice and running an action all dispatch on this key,
 * so a section's rows can be added to or reordered without touching any of
 * that code. Sections are just lists of keys - see kSections below.
 */
enum class SettingKey : int {
    /* Twitch */
    TwitchAccount,
    DefaultQuality,
    Chat,
    ChatTextSize,
    ThirdPartyEmotes,
    LiveNotifications,
    HomePreview,
    /* Controls & sound */
    Keyboard,
    NavigationSounds,
    ControllerLight,
    /* System */
    DebugOverlay,
    ClearHistory,
    About,
    Quit,
};

/*
 * One description of a setting, independent of how it is drawn.
 *
 * A VALUE setting carries every choice it has, not just the current one, so the
 * row can expand in place and show the user what actually exists.
 */
struct SettingDef {
    SettingKey  key;
    const char* title;
    const char* detail;
    const char* icon;
    int         kind;
    bool        toggle_on;
    const char* badge;
    int         opt_count;
    int         opt_current;
    const char* opt_label[kMaxOptions];
    bool        disabled;   /* depends on another setting: dimmed, Cross does nothing */
};

const SettingKey kTwitchKeys[] = {
    SettingKey::TwitchAccount,    SettingKey::DefaultQuality,    SettingKey::Chat,
    SettingKey::ChatTextSize,     SettingKey::ThirdPartyEmotes,  SettingKey::LiveNotifications,
    SettingKey::HomePreview,
};
const SettingKey kControlsKeys[] = {
    SettingKey::Keyboard, SettingKey::NavigationSounds, SettingKey::ControllerLight,
};
/* Debug overlay (the frame rate on screen) only in dev builds, the ones
 * packaged with --usb-remote. A release build has no row for it and keeps the
 * counter off (evo_fps_counter_enabled in Bridge.cpp). */
const SettingKey kSystemKeys[] = {
#if defined(EVO_USB_REMOTE)
    SettingKey::DebugOverlay,
#endif
    SettingKey::ClearHistory, SettingKey::About, SettingKey::Quit,
};

struct SectionSpec {
    const char*       title;
    const char*       subtitle;
    const char*       name;     /* screen name for the lifecycle FSM */
    ScreenId          screen;
    const SettingKey* keys;
    int               count;
};

template <std::size_t N>
constexpr SectionSpec makeSection(const char* title, const char* subtitle, const char* name,
                                  ScreenId screen, const SettingKey (&keys)[N]) {
    static_assert(N <= kMaxDefs, "settings section holds more than kMaxDefs settings");
    return {title, subtitle, name, screen, keys, static_cast<int>(N)};
}

/* Order here is the sidebar order in settings.rml (sb-0 .. sb-2) and in the
 * kit's evo_hui_settings.cpp. */
const SectionSpec kSections[] = {
    makeSection("Twitch", "Account, quality & chat",
                "SettingsTwitchScreen", ScreenId::SettingsTwitch, kTwitchKeys),
    makeSection("Controls & sound", "Keyboard, sounds & controller light",
                "SettingsControlsScreen", ScreenId::SettingsControls, kControlsKeys),
    makeSection("System", "History, about & quit",
                "SettingsSystemScreen", ScreenId::SettingsSystem, kSystemKeys),
};
constexpr int kSectionCount = static_cast<int>(sizeof kSections / sizeof kSections[0]);
static_assert(kSectionCount == EVO_RMLUI_SETTINGS_SECTIONS,
              "settings.rml and the kit draw EVO_RMLUI_SETTINGS_SECTIONS sections");

int sectionForScreen(ScreenId id) {
    for (int s = 0; s < kSectionCount; ++s)
        if (kSections[s].screen == id) return s;
    return 0;
}

/* The Default quality choices as Settings shows them, in twitch::kQualities
 * order. */
const char* const kQualityLabels[] = {
    "Auto", "Source", "1080p60", "720p60", "720p", "480p", "360p", "160p", "Audio only",
};
static_assert(sizeof kQualityLabels / sizeof kQualityLabels[0] == twitch::kQualityCount,
              "one label per twitch::kQualities entry");

/* Chat's choices, in twitch::ChatMode order, worded like the player's R1 toast. */
const char* const kChatLabels[] = {"Next to the video", "Over the video", "Off"};

/* Chat text size's choices, in twitch::ChatTextSize order. */
const char* const kChatTextSizeLabels[] = {"Small", "Medium", "Large"};

/* Keyboard's choices: 0 the PS5's own, 1 the app's on-screen one. */
const char* const kKeyboardLabels[] = {"PS5 keyboard", "On-screen keyboard"};

int currentQuality() {
    const std::string& q = evo_twitch::settings().quality;
    for (int i = 0; i < twitch::kQualityCount; ++i)
        if (q == twitch::kQualities[i]) return i;
    return 0;
}

SettingDef toggleDef(SettingKey key, const char* title, const char* detail,
                     const char* icon, bool on) {
    SettingDef d{};
    d.key = key; d.title = title; d.detail = detail; d.icon = icon;
    d.kind = EVO_RMLUI_ROW_TOGGLE; d.toggle_on = on; d.badge = "";
    return d;
}

SettingDef actionDef(SettingKey key, const char* title, const char* detail,
                     const char* icon, const char* badge) {
    SettingDef d{};
    d.key = key; d.title = title; d.detail = detail; d.icon = icon;
    d.kind = EVO_RMLUI_ROW_ACTION; d.badge = badge;
    return d;
}

/* Fills opt_label[] from `labels`. */
SettingDef valueDef(SettingKey key, const char* title, const char* detail,
                    const char* icon, const char* const* labels, int count, int current) {
    SettingDef d{};
    d.key = key; d.title = title; d.detail = detail; d.icon = icon;
    d.kind = EVO_RMLUI_ROW_VALUE; d.badge = "";
    d.opt_count = count; d.opt_current = current;
    for (int i = 0; i < count && i < kMaxOptions; ++i) d.opt_label[i] = labels[i];
    return d;
}

/* The logged-in user as the row shows them. The kit's fonts stop after
 * Cyrillic (U+04FF, whose UTF-8 lead byte is 0xD3), and a Twitch display
 * name can be in any script (a localised one in Armenian, Hebrew, Arabic or
 * Japanese, say), so a name with anything past that is shown as the login,
 * which always draws. */
const char* twitchAccountName(const evo_twitch::AuthView& v) {
    for (unsigned char c : v.displayName)
        if (c >= 0xD4) return v.login.c_str();
    return v.displayName.empty() ? v.login.c_str() : v.displayName.c_str();
}

SettingDef makeDef(SettingKey key, ISettingsService* settings) {
    SettingDef d{};
    switch (key) {
    /* Logged out: Cross opens the login screen. Logged in: the name, and
     * Cross asks "Log out of Twitch?" (SettingsSectionScreen). While the
     * saved login is still being read it says so rather than "not logged
     * in"; Cross then opens the login screen, which waits for the answer. A
     * VALUE row with no choices, so the kit draws its badge and it never
     * expands. */
    case SettingKey::TwitchAccount: {
        const evo_twitch::AuthView& v = evo_twitch::auth_view();
        d = valueDef(key, "Twitch account", "Press Cross to log in", "../icons/icon_listener.png",
                     nullptr, 0, -1);
        if (v.state == evo_twitch::AuthState::LoggedIn) {
            /* A login from before clips: only a new one brings them. */
            d.detail = evo_twitch::auth_has_scope(v, twitch::kClipScope)
                           ? "Logged in. Press Cross to log out"
                           : "Log in again to make clips. Press Cross to log out";
            d.badge = twitchAccountName(v);
        } else if (v.state == evo_twitch::AuthState::Unknown) {
            d.detail = "Looking for a saved login";
            d.badge = "Checking";
        } else {
            d.badge = "Not logged in";
        }
        break;
    }

    /* Stored only; the launcher reads it when a stream starts. */
    case SettingKey::DefaultQuality:
        d = valueDef(key, "Default quality", "Streams start in this quality. Auto picks the best one",
                     "../icons/icon_tv.png", kQualityLabels, twitch::kQualityCount, currentQuality());
        break;

    case SettingKey::Chat:
        d = valueDef(key, "Chat", "Where chat goes while a stream plays. R1 in the player changes it too",
                     "../icons/icon_twitch_line.png", kChatLabels, 3,
                     static_cast<int>(evo_twitch::settings().chatMode));
        break;

    case SettingKey::ChatTextSize:
        d = valueDef(key, "Chat text size", "For chat next to the video and over it",
                     "../icons/icon_text_size.png", kChatTextSizeLabels, 3,
                     static_cast<int>(evo_twitch::settings().chatTextSize));
        break;

    /* Read by the chat service every frame: off, nothing more is fetched
     * and new lines show these emotes as their names. */
    case SettingKey::ThirdPartyEmotes:
        d = toggleDef(key, "Third-party emotes", "7TV, BetterTTV and FrankerFaceZ emotes in chat",
                      "../icons/icon_emote.png", evo_twitch::settings().thirdPartyEmotes);
        break;

    /* Read by the go-live notifications every frame (evo_twitch_alerts.h). */
    case SettingKey::LiveNotifications:
        d = toggleDef(key, "Live notifications", "A note when a channel you follow goes live",
                      "../icons/icon_bell.png", evo_twitch::settings().liveNotifications);
        break;

    /* Read by Home every frame: off, its hero shows a still picture. */
    case SettingKey::HomePreview:
        d = toggleDef(key, "Live preview on Home", "The top stream plays without sound at the top of Home",
                      "../icons/icon_tv.png", evo_twitch::settings().homePreview);
        break;

    case SettingKey::Keyboard:
        d = valueDef(key, "Keyboard", "For search and chat messages", "../icons/icon_keyboard.png",
                     kKeyboardLabels, 2, settings->getKeyboardType() == EVO_KEYBOARD_TYPE_VIRTUAL ? 1 : 0);
        break;

    case SettingKey::NavigationSounds:
        d = toggleDef(key, "Navigation sounds", "A sound on each move and press",
                      "../icons/icon_volume.png", settings->isSoundFeedbackEnabled());
        break;

    case SettingKey::ControllerLight:
        d = toggleDef(key, "Controller light", "The controller glows Twitch purple",
                      "../icons/icon_gamepad.png", settings->isLightbarFeedbackEnabled());
        break;

    case SettingKey::DebugOverlay:
        d = toggleDef(key, "Debug overlay", "Shows the frame rate on screen",
                      "../icons/icon_activity.png", settings->isDebugOverlayEnabled());
        break;

    /* Asks first (SettingsSectionScreen), then forgets the channels watched
     * and every video's progress; the recent searches stay. */
    case SettingKey::ClearHistory:
        d = actionDef(key, "Clear watch history", "Recently watched and Continue watching",
                      "../icons/icon_history.png", "Clear");
        break;

    /* The version, the license notice and the credits (GPLv3 5(d)). */
    case SettingKey::About:
        d = actionDef(key, "About", "Version, licenses and credits",
                      "../icons/icon_about_support.png", "Open");
        break;

    case SettingKey::Quit:
        d = actionDef(key, "Quit", "Stops everything so you can close the app with the PS button",
                      "../icons/icon_power.png", "Quit");
        break;
    }

    /* A collapsed VALUE row shows its current choice as the badge (the
     * Twitch account, which has no choices, set its own). */
    if (d.kind == EVO_RMLUI_ROW_VALUE && d.opt_count > 0) {
        const int c = d.opt_current;
        d.badge = (c >= 0 && c < d.opt_count && d.opt_label[c]) ? d.opt_label[c] : "";
    }
    return d;
}

int buildSectionDefs(int section, SettingDef* d) {
    ISettingsService* settings = Application::getInstance().getSettingsService();
    if (!settings || section < 0 || section >= kSectionCount) return 0;
    const SectionSpec& spec = kSections[section];
    for (int i = 0; i < spec.count; ++i) d[i] = makeDef(spec.keys[i], settings);
    return spec.count;
}

/* One visible line: a setting, or a choice under an expanded setting. */
struct DisplaySlot { int def; int opt; };   /* opt < 0 = the setting row itself */

int buildSlots(const SettingDef* d, int n, int expanded, DisplaySlot* out) {
    int m = 0;
    for (int i = 0; i < n && m < EVO_RMLUI_SETTINGS_ROWS; ++i) {
        out[m].def = i; out[m].opt = -1; ++m;
        if (i == expanded && d[i].kind == EVO_RMLUI_ROW_VALUE && !d[i].disabled) {
            for (int o = 0; o < d[i].opt_count && m < EVO_RMLUI_SETTINGS_ROWS; ++o) {
                out[m].def = i; out[m].opt = o; ++m;
            }
        }
    }
    return m;
}

/* Fills the params for one section. cursor < 0 = the sidebar owns the cursor,
 * so nothing in the detail pane is highlighted. */
int fillSection(int section, int expanded, int cursor, evo_rmlui_settings_params_t& p) {
    SettingDef d[kMaxDefs];
    DisplaySlot slots[EVO_RMLUI_SETTINGS_ROWS];
    const int n = buildSectionDefs(section, d);
    const int m = buildSlots(d, n, expanded, slots);

    if (section >= 0 && section < kSectionCount) {
        p.title = kSections[section].title;
        p.subtitle = kSections[section].subtitle;
    }

    for (int i = 0; i < m; ++i) {
        const SettingDef& def = d[slots[i].def];
        const int opt = slots[i].opt;
        if (opt < 0) {
            p.rows[i].title = def.title;
            p.rows[i].detail = def.detail;
            p.rows[i].icon_path = def.icon;
            p.rows[i].badge = def.badge;
            p.rows[i].kind = def.kind;
            p.rows[i].toggle_on = def.toggle_on ? 1 : 0;
            p.rows[i].has_chevron = (def.kind != EVO_RMLUI_ROW_TOGGLE) ? 1 : 0;
            p.rows[i].is_disabled = def.disabled ? 1 : 0;
        } else {
            p.rows[i].title = def.opt_label[opt];
            p.rows[i].detail = "";
            p.rows[i].icon_path = "";
            p.rows[i].badge = "";
            p.rows[i].kind = EVO_RMLUI_ROW_OPTION;
            p.rows[i].toggle_on = (opt == def.opt_current) ? 1 : 0;
            p.rows[i].has_chevron = 0;
        }
        p.rows[i].is_focused = (i == cursor) ? 1 : 0;
    }

    static char s_counter[32];
    std::snprintf(s_counter, sizeof s_counter, "%d setting%s", n, n == 1 ? "" : "s");
    p.counter = s_counter;
    p.row_count = m;
    return m;
}

void toggleSetting(SettingKey key) {
    ISettingsService* st = Application::getInstance().getSettingsService();
    if (!st) return;
    switch (key) {
    case SettingKey::NavigationSounds: st->setSoundFeedbackEnabled(!st->isSoundFeedbackEnabled()); break;
    case SettingKey::ControllerLight:  st->setLightbarFeedbackEnabled(!st->isLightbarFeedbackEnabled()); break;
    case SettingKey::DebugOverlay:     st->setDebugOverlayEnabled(!st->isDebugOverlayEnabled()); break;
    case SettingKey::ThirdPartyEmotes:
        evo_twitch::set_third_party_emotes(!evo_twitch::settings().thirdPartyEmotes);
        break;
    case SettingKey::LiveNotifications:
        evo_twitch::set_live_notifications(!evo_twitch::settings().liveNotifications);
        break;
    case SettingKey::HomePreview:
        evo_twitch::set_home_preview(!evo_twitch::settings().homePreview);
        break;
    default: return;
    }
    evo_feedback(EVO_FB_TOGGLE);
}

void applyOption(SettingKey key, int opt) {
    ISettingsService* st = Application::getInstance().getSettingsService();
    if (!st || opt < 0) return;
    switch (key) {
    case SettingKey::DefaultQuality:
        if (opt >= twitch::kQualityCount) return;
        evo_twitch::set_quality(twitch::kQualities[opt]);
        break;
    case SettingKey::Chat:
        if (opt > static_cast<int>(twitch::ChatMode::Off)) return;
        evo_twitch::set_chat_mode(static_cast<twitch::ChatMode>(opt));
        break;
    case SettingKey::ChatTextSize:
        if (opt > static_cast<int>(twitch::ChatTextSize::Large)) return;
        evo_twitch::set_chat_text_size(static_cast<twitch::ChatTextSize>(opt));
        break;
    case SettingKey::Keyboard:
        st->setKeyboardType(opt == 1 ? EVO_KEYBOARD_TYPE_VIRTUAL : EVO_KEYBOARD_TYPE_NATIVE);
        break;
    default: return;
    }
    evo_feedback(EVO_FB_TOGGLE);
}

bool runAction(const SettingDef& def) {
    if (def.disabled) {
        evo_feedback(EVO_FB_BOUNDARY);
        return false;
    }

    switch (def.key) {
    case SettingKey::About:
        if (auto sm = Application::getInstance().getScreenManager()) {
            evo_feedback(EVO_FB_OPEN);
            sm->navigateTo(ScreenId::AboutSupport);
            return true;
        }
        return false;
    /*
     * Soft close, not exit.
     *
     * Closing from the PS button kills the process outright, so the frame loop
     * never returns and Application::shutdown() never runs - meaning the GPU is
     * never drained and the scanout registration and direct-memory pool are
     * left for the kernel to reclaim underneath a GPU that may still be
     * mid-submit. requestExit() just clears m_running, so the loop finishes the
     * frame it is on and exits through the ordinary shutdown path like any
     * other return from run().
     *
     * This used to call requestExit(), which left through shutdown() and
     * returned from main(). The teardown itself worked - the breadcrumbs ran
     * all the way to "shutdown: complete" - but the libc exit path after it
     * crashed every time, and a PS5 app is not really expected to terminate
     * itself anyway: no retail game ships a quit menu. So it now releases
     * everything and parks instead, and the user closes it from the switcher.
     * See Application::requestSoftClose().
     */
    case SettingKey::Quit:
        evo_feedback(EVO_FB_OPEN);
        evo_boot_log("settings: QUIT selected - soft close");
        evo_boot_log_flush();
        Application::getInstance().requestSoftClose();
        return true;
    default:
        return false;
    }
}

void saveSettings() {
    if (auto settings = Application::getInstance().getSettingsService()) {
        settings->saveSettings();
    }
}

} // namespace

// =============================================================================
// SettingsScreen (Top-level Category Selector)
// =============================================================================

SettingsScreen::SettingsScreen()
    : StatefulScreen("SettingsScreen") {
}

void SettingsScreen::onEnter() {
    StatefulScreen::onEnter();
    /* Deliberately keeps m_selectedIndex: Circle out of a section returns the
     * cursor to that section, the way a sidebar should behave. */
}

void SettingsScreen::onExit() {
    StatefulScreen::onExit();
    saveSettings();
}

void SettingsScreen::navigate(int delta) {
    constexpr int totalRows = kSectionCount;
    m_selectedIndex += delta;
    if (m_selectedIndex < 0) {
        m_selectedIndex = 0;
        evo_feedback(EVO_FB_BOUNDARY);
    } else if (m_selectedIndex >= totalRows) {
        m_selectedIndex = totalRows - 1;
        evo_feedback(EVO_FB_BOUNDARY);
    } else {
        evo_feedback(EVO_FB_MOVE);
    }
}

void SettingsScreen::activateSelection() {
    auto screenMgr = Application::getInstance().getScreenManager();
    if (!screenMgr) return;

    evo_feedback(EVO_FB_CONFIRM);
    screenMgr->navigateTo(kSections[m_selectedIndex].screen);
}

bool SettingsScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released) {
    (void)held;
    (void)released;

    if (pressed & PadButtons::Up) {
        navigate(-1);
        return true;
    }
    if (pressed & PadButtons::Down) {
        navigate(1);
        return true;
    }
    if (pressed & PadButtons::Left) {
        if (auto screenMgr = Application::getInstance().getScreenManager()) {
            screenMgr->setRailFocused(true);
            return true;
        }
    }
    if ((pressed & PadButtons::Right) || (pressed & PadButtons::Cross)) {
        /* Right steps from the sidebar into the detail pane, same as Cross. */
        activateSelection();
        return true;
    }
    if (pressed & PadButtons::Circle) {
        evo_feedback(EVO_FB_CANCEL);
        if (auto screenMgr = Application::getInstance().getScreenManager()) {
            screenMgr->navigateTo(ScreenId::MainMenu);
        }
        return true;
    }

    return false;
}

void SettingsScreen::update(double deltaMs) {
    (void)deltaMs;
}

void SettingsScreen::render(uint32_t* framebuffer, int width, int height) {
    evo_rmlui_settings_params_t params;
    std::memset(&params, 0, sizeof(params));

    bool railFocused = false;
    if (auto screenMgr = Application::getInstance().getScreenManager()) {
        railFocused = screenMgr->isRailFocused();
    }

    params.rail_active_idx = 5;
    params.rail_focused = railFocused ? 1 : 0;
    params.section_active = m_selectedIndex;
    params.sidebar_focused = railFocused ? 0 : 1;

    /* The sidebar is the page here, so the detail pane previews the highlighted
     * section's real options rather than repeating the section list. Nothing is
     * expanded and no row is focused - the cursor lives in the sidebar. */
    fillSection(m_selectedIndex, -1, -1, params);

    evo_rmlui_update_settings(&params);
    evo_rmlui_render_settings(framebuffer, width, height);
}

// =============================================================================
// SettingsSectionScreen (one per section in kSections)
// =============================================================================

SettingsSectionScreen::SettingsSectionScreen(ScreenId id)
    : StatefulScreen(kSections[sectionForScreen(id)].name),
      m_id(id),
      m_section(sectionForScreen(id)) {
}

void SettingsSectionScreen::onExit() {
    StatefulScreen::onExit();
    m_confirm = Confirm::None;
    saveSettings();
}

void SettingsSectionScreen::onEnter() {
    StatefulScreen::onEnter();
    m_selectedIndex = 0;
    m_expandedIndex = -1;
    m_confirm = Confirm::None;
}

void SettingsSectionScreen::navigate(int delta) {
    SettingDef d[kMaxDefs];
    DisplaySlot slots[EVO_RMLUI_SETTINGS_ROWS];
    const int total = buildSlots(d, buildSectionDefs(m_section, d), m_expandedIndex, slots);
    if (total <= 0) return;
    m_selectedIndex += delta;
    if (m_selectedIndex < 0) {
        m_selectedIndex = 0;
        evo_feedback(EVO_FB_BOUNDARY);
    } else if (m_selectedIndex >= total) {
        m_selectedIndex = total - 1;
        evo_feedback(EVO_FB_BOUNDARY);
    } else {
        evo_feedback(EVO_FB_MOVE);
    }
}

void SettingsSectionScreen::activateSelection() {
    SettingDef d[kMaxDefs];
    DisplaySlot slots[EVO_RMLUI_SETTINGS_ROWS];
    const int m = buildSlots(d, buildSectionDefs(m_section, d), m_expandedIndex, slots);
    if (m_selectedIndex < 0 || m_selectedIndex >= m) return;
    const int def = slots[m_selectedIndex].def;
    const int opt = slots[m_selectedIndex].opt;

    if (opt >= 0) {
        /* A choice under an expanded setting: apply it and collapse, leaving
         * the cursor on the setting itself. */
        applyOption(d[def].key, opt);
        m_expandedIndex = -1;
        m_selectedIndex = def;
        return;
    }

    if (d[def].key == SettingKey::TwitchAccount) {
        evo_feedback(EVO_FB_OPEN);
        if (evo_twitch::auth_view().state == evo_twitch::AuthState::LoggedIn) {
            m_confirm = Confirm::Logout;
            m_confirmButton = 0;  /* Cancel first, as for a delete */
        } else if (auto screenMgr = Application::getInstance().getScreenManager()) {
            screenMgr->navigateTo(ScreenId::TwitchLogin);
        }
        return;
    }
    if (d[def].key == SettingKey::ClearHistory) {
        evo_feedback(EVO_FB_OPEN);
        m_confirm = Confirm::ClearHistory;
        m_confirmButton = 0;  /* Cancel first, as for a delete */
        return;
    }

    /* A disabled row does nothing but the boundary buzz (runAction). */
    switch (d[def].disabled ? EVO_RMLUI_ROW_ACTION : d[def].kind) {
    case EVO_RMLUI_ROW_TOGGLE:
        toggleSetting(d[def].key);
        break;
    case EVO_RMLUI_ROW_VALUE:
        /* Expand in place so every choice is visible, collapse if already open. */
        m_expandedIndex = (m_expandedIndex == def) ? -1 : def;
        m_selectedIndex = def;
        evo_feedback(EVO_FB_CONFIRM);
        break;
    default:
        runAction(d[def]);
        break;
    }
}

bool SettingsSectionScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released) {
    (void)held;
    (void)released;

    /* "Log out of Twitch?" and "Clear watch history?" hold the pad while
     * they are up. */
    if (m_confirm != Confirm::None) {
        if (pressed & (PadButtons::Left | PadButtons::Right)) {
            m_confirmButton = m_confirmButton == 0 ? 1 : 0;
            evo_feedback(EVO_FB_MOVE);
        } else if (pressed & PadButtons::Circle) {
            m_confirm = Confirm::None;
            evo_feedback(EVO_FB_CANCEL);
        } else if (pressed & PadButtons::Cross) {
            const Confirm what = m_confirm;
            m_confirm = Confirm::None;
            if (m_confirmButton != 1) {
                evo_feedback(EVO_FB_CANCEL);
            } else if (what == Confirm::Logout) {
                evo_twitch::auth_logout();
                evo_feedback(EVO_FB_CONFIRM);
            } else {
                /* The channels watched, and every video's progress with
                 * Continue watching; both files are written at once. */
                evo_twitch::history_clear_channels();
                evo_twitch::progress_clear();
                evo_twitch::progress_save();
                evo_feedback(EVO_FB_CONFIRM);
                toast_success("Watch history cleared", nullptr);
            }
        }
        return true;
    }

    /* The D-pad only ever moves the cursor. Values change on Cross alone -
     * Left/Right used to edit the highlighted setting, which meant walking the
     * menu silently rewrote your settings. */
    if (pressed & PadButtons::Up) {
        navigate(-1);
        return true;
    }
    if (pressed & PadButtons::Down) {
        navigate(1);
        return true;
    }
    if (pressed & PadButtons::Left) {
        if (m_expandedIndex >= 0) {
            m_expandedIndex = -1;
            evo_feedback(EVO_FB_CANCEL);
            return true;
        }
        if (auto screenMgr = Application::getInstance().getScreenManager()) {
            evo_feedback(EVO_FB_CANCEL);
            screenMgr->navigateTo(ScreenId::Settings);
            return true;
        }
        return true;
    }
    if (pressed & PadButtons::Right) {
        SettingDef d[kMaxDefs];
        DisplaySlot slots[EVO_RMLUI_SETTINGS_ROWS];
        const int m = buildSlots(d, buildSectionDefs(m_section, d), m_expandedIndex, slots);
        if (m_selectedIndex >= 0 && m_selectedIndex < m && slots[m_selectedIndex].opt < 0) {
            const int def = slots[m_selectedIndex].def;
            if (d[def].kind == EVO_RMLUI_ROW_VALUE && d[def].opt_count > 0 && !d[def].disabled &&
                m_expandedIndex != def) {
                m_expandedIndex = def;
                evo_feedback(EVO_FB_CONFIRM);
            }
        }
        return true;
    }
    if (pressed & PadButtons::Cross) {
        activateSelection();
        return true;
    }
    if (pressed & PadButtons::Circle) {
        if (m_expandedIndex >= 0) {
            m_expandedIndex = -1;
            evo_feedback(EVO_FB_CANCEL);
            return true;
        }
        evo_feedback(EVO_FB_CANCEL);
        if (auto screenMgr = Application::getInstance().getScreenManager()) {
            screenMgr->navigateTo(ScreenId::Settings);
        }
        return true;
    }

    return false;
}

void SettingsSectionScreen::render(uint32_t* framebuffer, int width, int height) {
    evo_rmlui_settings_params_t params;
    std::memset(&params, 0, sizeof(params));

    params.rail_active_idx = 5;
    params.rail_focused = 0;
    params.section_active = m_section;
    params.sidebar_focused = 0;

    fillSection(m_section, m_expandedIndex, m_selectedIndex, params);

    evo_rmlui_update_settings(&params);
    evo_rmlui_render_settings(framebuffer, width, height);

    /* Logged out meanwhile (the login ran out): nothing left to ask. */
    if (m_confirm == Confirm::Logout && evo_twitch::auth_view().state != evo_twitch::AuthState::LoggedIn)
        m_confirm = Confirm::None;
    if (m_confirm != Confirm::None) {
        const bool logout = m_confirm == Confirm::Logout;
        evo_rmlui_dialog_params_t dlg;
        std::memset(&dlg, 0, sizeof(dlg));
        dlg.eyebrow = logout ? "TWITCH ACCOUNT" : "WATCH HISTORY";
        dlg.title = logout ? "Log out of Twitch?" : "Clear watch history?";
        dlg.detail = logout ? "This PS5 forgets your login. You can log in again any time."
                            : "Recently watched and Continue watching are emptied. Your searches stay.";
        dlg.progress_pct = -1.0;
        dlg.action_count = 2;
        dlg.focused_action = m_confirmButton;
        dlg.actions[0].label = "CANCEL";
        dlg.actions[0].icon_path = "../icons/btn_circle.png";
        dlg.actions[0].is_primary = m_confirmButton == 0 ? 1 : 0;
        dlg.actions[1].label = logout ? "LOG OUT" : "CLEAR";
        dlg.actions[1].icon_path = "../icons/btn_cross.png";
        dlg.actions[1].is_primary = m_confirmButton == 1 ? 1 : 0;
        evo_rmlui_update_dialog(&dlg);
        evo_rmlui_render_dialog(framebuffer, width, height);
    }
}

void SettingsSectionScreen::update(double deltaMs) {
    (void)deltaMs;
}

} // namespace evo
