#include "evo/screens/ModalDialogScreen.hpp"
#include "evo/Application.hpp"
#include "evo_rmlui_bridge.h"
#include "evo_feedback.h"

#include <cstring>

namespace evo {

ModalDialogScreen::ModalDialogScreen()
    : StatefulScreen("ModalDialogScreen")
    , m_modalFsm(ModalDialogState::Inactive, "ModalDialogFSM")
{
    initModalStateMachine();
}

void ModalDialogScreen::initModalStateMachine() {
    m_modalFsm
        .addState(ModalDialogState::Inactive, "Inactive")
        .addState(ModalDialogState::Prompting, "Prompting")
        .addState(ModalDialogState::Confirmed, "Confirmed")
        .addState(ModalDialogState::Cancelled, "Cancelled");

    m_modalFsm
        .addTransition(ModalDialogState::Inactive, ModalDialogEvent::Open, ModalDialogState::Prompting)
        .addTransition(ModalDialogState::Prompting, ModalDialogEvent::Confirm, ModalDialogState::Confirmed)
        .addTransition(ModalDialogState::Prompting, ModalDialogEvent::Cancel, ModalDialogState::Cancelled)
        .addTransition(ModalDialogState::Confirmed, ModalDialogEvent::Dismiss, ModalDialogState::Inactive)
        .addTransition(ModalDialogState::Cancelled, ModalDialogEvent::Dismiss, ModalDialogState::Inactive)
        .addTransition(ModalDialogState::Prompting, ModalDialogEvent::Dismiss, ModalDialogState::Inactive);
}

ScreenId ModalDialogScreen::getScreenId() const {
    return ScreenId::ExitConfirm;
}

void ModalDialogScreen::onEnter() {
    StatefulScreen::onEnter();
    m_modalFsm.postEvent(ModalDialogEvent::Open);
    m_focusedButton = 1; // STOP
}

void ModalDialogScreen::onExit() {
    StatefulScreen::onExit();
    m_modalFsm.postEvent(ModalDialogEvent::Dismiss);
}

void ModalDialogScreen::executeAction(int actionIndex) {
    auto playback = Application::getInstance().getPlaybackController();
    auto screenMgr = Application::getInstance().getScreenManager();
    if (!screenMgr) return;

    if (actionIndex == 0) {
        // "KEEP WATCHING"
        evo_feedback(EVO_FB_CANCEL);
        screenMgr->navigateBack(ScreenId::Player);
    } else {
        // "STOP"
        evo_feedback(EVO_FB_CONFIRM);
        if (playback) {
            playback->stopPlayback();
        }
        screenMgr->returnFromPlayback();
    }
}

bool ModalDialogScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released) {
    (void)held;
    (void)released;

    if (pressed & (PadButtons::Left | PadButtons::Right | PadButtons::Up | PadButtons::Down)) {
        m_focusedButton = (m_focusedButton == 0) ? 1 : 0;
        evo_feedback(EVO_FB_MOVE);
        return true;
    }

    if (pressed & PadButtons::Cross) {
        executeAction(m_focusedButton);
        return true;
    }

    if (pressed & PadButtons::Circle) {
        evo_feedback(EVO_FB_CANCEL);
        if (auto sm = Application::getInstance().getScreenManager()) {
            sm->navigateBack(ScreenId::Player);
        }
        return true;
    }

    return false;
}

void ModalDialogScreen::update(double deltaMs) {
    StatefulScreen::update(deltaMs);
    m_modalFsm.update(deltaMs);
}

void ModalDialogScreen::render(uint32_t* framebuffer, int width, int height) {
    if (!evo_rmlui_is_initialized()) return;

    evo_rmlui_dialog_params_t dialog;
    std::memset(&dialog, 0, sizeof(dialog));

    dialog.eyebrow = "PLAYBACK";
    dialog.title = "Stop playback?";
    dialog.progress_pct = -1.0;
    dialog.action_count = 2;
    dialog.focused_action = m_focusedButton;

    dialog.actions[0].label = "KEEP WATCHING";
    dialog.actions[0].icon_path = "/assets/icons/btn_circle.png";
    dialog.actions[0].is_primary = (m_focusedButton == 0) ? 1 : 0;

    dialog.actions[1].label = "STOP";
    dialog.actions[1].icon_path = "/assets/icons/btn_cross.png";
    dialog.actions[1].is_primary = (m_focusedButton == 1) ? 1 : 0;

    evo_rmlui_update_dialog(&dialog);
    evo_rmlui_render_dialog(framebuffer, width, height);
}

} // namespace evo
