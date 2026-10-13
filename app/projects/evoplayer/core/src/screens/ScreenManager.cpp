#include "evo/screens/ScreenManager.hpp"
#include "evo/Application.hpp"
#include "evo_rmlui_bridge.h"
#include "evo_feedback.h"

extern "C" {
extern int screen;
void evo_sync_rmlui_nav(int section, int rail_focused, int rail_index, int visible);
}

namespace evo {

ScreenManager::ScreenManager()
    : m_navFsm(NavigationState::ScreenActive, "NavigationFSM")
{
    initStateMachine();
}

void ScreenManager::initStateMachine() {
    m_navFsm
        .addState(NavigationState::ScreenActive, "ScreenActive")
        .addState(NavigationState::RailFocused, "RailFocused")
        .addState(NavigationState::ModalActive, "ModalActive");

    m_navFsm
        .addTransition(NavigationState::ScreenActive, NavigationEvent::FocusRail, NavigationState::RailFocused)
        .addTransition(NavigationState::ScreenActive, NavigationEvent::NavigateScreen, NavigationState::ScreenActive)
        .addTransition(NavigationState::ScreenActive, NavigationEvent::OpenModal, NavigationState::ModalActive)
        .addTransition(NavigationState::RailFocused, NavigationEvent::UnfocusRail, NavigationState::ScreenActive)
        .addTransition(NavigationState::RailFocused, NavigationEvent::NavigateScreen, NavigationState::ScreenActive)
        .addTransition(NavigationState::ModalActive, NavigationEvent::CloseModal, NavigationState::ScreenActive)
        .addTransition(NavigationState::ModalActive, NavigationEvent::NavigateScreen, NavigationState::ScreenActive);
}

void ScreenManager::setModalDialogActive(bool active) {
    m_modalActive = active;
    if (active) {
        m_navFsm.postEvent(NavigationEvent::OpenModal);
    } else {
        m_navFsm.postEvent(NavigationEvent::CloseModal);
    }
}

void ScreenManager::registerScreen(std::unique_ptr<IScreen> screenPtr) {
    if (screenPtr) {
        int id = static_cast<int>(screenPtr->getScreenId());
        m_screens[id] = std::move(screenPtr);
    }
}

IScreen* ScreenManager::getCurrentScreen() const {
    auto it = m_screens.find(static_cast<int>(m_currentScreenId));
    if (it != m_screens.end()) {
        return it->second.get();
    }
    return nullptr;
}

IScreen* ScreenManager::getScreen(ScreenId screenId) const {
    auto it = m_screens.find(static_cast<int>(screenId));
    if (it != m_screens.end()) {
        return it->second.get();
    }
    return nullptr;
}

/*
 * The Twitch rail: Home, Following, Browse, Search, Settings, then QUIT (see
 * activateRail). The kit's rail_items() (evo_hui_screens.cpp) and the RmlUi
 * fallback's navbar.rml list them in the same order. About opens from
 * Settings and counts as Settings. A channel page belongs to the section it
 * was opened from (setChannelSection).
 */
int ScreenManager::getSectionForScreen(ScreenId screenId) const {
    switch (screenId) {
        case ScreenId::TwitchHome:      return 0; // Home
        case ScreenId::TwitchFollowing: return 1; // Following
        case ScreenId::TwitchBrowse:
        case ScreenId::TwitchCategory:  return 2; // Browse
        case ScreenId::TwitchSearch:    return 3; // Search
        case ScreenId::TwitchChannel:   return m_channelSection;
        case ScreenId::Settings:
        case ScreenId::SettingsTwitch:
        case ScreenId::SettingsControls:
        case ScreenId::SettingsSystem:
        case ScreenId::AboutSupport:    return 4; // Settings (About is under System)
        default:                        return -1; // Non-rail screens
    }
}

void ScreenManager::setChannelSection(int section) {
    m_channelSection = section >= 0 && section < railSectionCount() ? section : 0;
}

bool ScreenManager::isNavRailVisible() const {
    return getSectionForScreen(m_currentScreenId) >= 0;
}

void ScreenManager::setRailFocused(bool focused) {
    if (!isNavRailVisible() && focused) return;
    if (m_railFocused == focused) return;

    m_railFocused = focused;
    if (m_railFocused) {
        m_navFsm.postEvent(NavigationEvent::FocusRail);
        int sec = getSectionForScreen(m_currentScreenId);
        m_railIndex = (sec >= 0) ? sec : 0;
        evo_feedback(EVO_FB_OPEN);
    } else {
        m_navFsm.postEvent(NavigationEvent::UnfocusRail);
        evo_feedback(EVO_FB_CANCEL);
    }
    syncNavRail();
}

void ScreenManager::stepRail(int delta) {
    /* The sections, plus one entry after them: QUIT (see activateRail). */
    const int numEntries = railSectionCount() + 1;
    m_railIndex = (m_railIndex + delta) % numEntries;
    if (m_railIndex < 0) m_railIndex += numEntries;
    evo_feedback(EVO_FB_MOVE);
    syncNavRail();
}

ScreenId ScreenManager::getRootScreenForSection(int section) const {
    /* Must stay the inverse of getSectionForScreen() above (a channel page
     * is no section's root). */
    switch (section) {
        case 0: return ScreenId::TwitchHome;
        case 1: return ScreenId::TwitchFollowing;
        case 2: return ScreenId::TwitchBrowse;
        case 3: return ScreenId::TwitchSearch;
        case 4: return ScreenId::Settings;
        default: return ScreenId::TwitchHome;
    }
}

bool ScreenManager::isPlaybackScreen(ScreenId screenId) const {
    switch (screenId) {
        case ScreenId::Player:
        case ScreenId::ExitConfirm:
        case ScreenId::AudioTrackPicker:
            return true;
        default:
            return false;
    }
}

int ScreenManager::railSectionCount() const {
    return 5;
}

void ScreenManager::activateRail() {
    int targetSection = m_railIndex;
    m_railFocused = false;

    /* The entry after the last section is QUIT EVO: not a place to go but
     * the soft close - release everything, show the safe-to-close screen,
     * park. navbar.rml pins it to the foot of the rail. */
    if (targetSection == railSectionCount()) {
        evo_feedback(EVO_FB_OPEN);
        m_railIndex = 0;
        syncNavRail();
        Application::getInstance().requestSoftClose();
        return;
    }

    ScreenId targetRoot = getRootScreenForSection(targetSection);
    if (m_currentScreenId == targetRoot) {
        evo_feedback(EVO_FB_CANCEL);
        syncNavRail();
        return;
    }

    evo_feedback(EVO_FB_OPEN);
    navigateTo(targetRoot);
}

void ScreenManager::navigateTo(ScreenId screenId) {
    /* EVO's screens go "home" with MainMenu (Circle in Settings); home is
     * now the Twitch one. */
    if (screenId == ScreenId::MainMenu) screenId = ScreenId::TwitchHome;
    if (m_currentScreenId == screenId) {
        m_railFocused = false;
        syncNavRail();
        return;
    }

    if (auto current = getCurrentScreen()) {
        current->onExit();
    }

    m_history.push_back(screenId);
    m_currentScreenId = screenId;
    m_navFsm.postEvent(NavigationEvent::NavigateScreen);
    screen = static_cast<int>(screenId); // Sync global for legacy/RmlUi hooks

    m_railFocused = false;
    int sec = getSectionForScreen(screenId);
    if (sec >= 0) m_railIndex = sec;

    if (auto next = getCurrentScreen()) {
        next->onEnter();
    }

    syncNavRail();
}

bool ScreenManager::navigateBack() {
    if (m_history.size() <= 1) {
        return false;
    }

    if (auto current = getCurrentScreen()) {
        current->onExit();
    }

    m_history.pop_back();
    m_currentScreenId = m_history.back();
    screen = static_cast<int>(m_currentScreenId);

    m_railFocused = false;
    int sec = getSectionForScreen(m_currentScreenId);
    if (sec >= 0) m_railIndex = sec;

    if (auto next = getCurrentScreen()) {
        next->onEnter();
    }

    syncNavRail();
    return true;
}

bool ScreenManager::navigateBack(ScreenId fallbackScreen) {
    if (navigateBack()) {
        return true;
    }
    navigateTo(fallbackScreen);
    return false;
}

ScreenId ScreenManager::getPlaybackReturnScreen() const {
    for (auto it = m_history.rbegin(); it != m_history.rend(); ++it) {
        if (!isPlaybackScreen(*it)) {
            return *it;
        }
    }
    return ScreenId::TwitchHome;
}

void ScreenManager::returnFromPlayback() {
    ScreenId target = getPlaybackReturnScreen();

    if (auto current = getCurrentScreen()) {
        current->onExit();
    }

    // Prune all playback session screens from the history stack
    while (!m_history.empty() && isPlaybackScreen(m_history.back())) {
        m_history.pop_back();
    }

    // Ensure the destination screen is at the top of history
    if (m_history.empty() || m_history.back() != target) {
        m_history.push_back(target);
    }

    m_currentScreenId = target;
    m_navFsm.postEvent(NavigationEvent::NavigateScreen);
    screen = static_cast<int>(target);

    m_railFocused = false;
    int sec = getSectionForScreen(target);
    if (sec >= 0) m_railIndex = sec;

    if (auto next = getCurrentScreen()) {
        next->onEnter();
    }

    syncNavRail();
}

void ScreenManager::handleInput(uint32_t pressed, uint32_t held, uint32_t released) {
    if (m_railFocused) {
        if (pressed & PadButtons::Up) {
            stepRail(-1);
            return;
        }
        if (pressed & PadButtons::Down) {
            stepRail(1);
            return;
        }
        if (pressed & (PadButtons::Right | PadButtons::Circle)) {
            setRailFocused(false);
            return;
        }
        if (pressed & PadButtons::Cross) {
            activateRail();
            return;
        }
        return; // Absorb all other input while rail is focused
    }

    bool handled = false;
    if (auto current = getCurrentScreen()) {
        handled = current->handleInput(pressed, held, released);
    } else {
        if (pressed & PadButtons::Circle) {
            if (!navigateBack()) {
                navigateTo(ScreenId::MainMenu);
            }
            return;
        }
    }

    if (!handled && (pressed & PadButtons::Left) && isNavRailVisible()) {
        setRailFocused(true);
    }
}

void ScreenManager::update(double deltaMs) {
    m_navFsm.update(deltaMs);
    if (auto current = getCurrentScreen()) {
        current->update(deltaMs);
    }
}

void ScreenManager::render(uint32_t* framebuffer, int width, int height) {
    syncNavRail();
    if (auto current = getCurrentScreen()) {
        current->render(framebuffer, width, height);
    }
}

void ScreenManager::syncNavRail() {
    int section = getSectionForScreen(m_currentScreenId);
    bool visible = (section >= 0);
    if (!visible) section = 0;

    evo_sync_rmlui_nav(section, m_railFocused ? 1 : 0, m_railIndex, visible ? 1 : 0);
}

} // namespace evo
