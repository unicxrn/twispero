#ifndef EVO_MODAL_DIALOG_SCREEN_HPP
#define EVO_MODAL_DIALOG_SCREEN_HPP

#include "evo/screens/StatefulScreen.hpp"
#include "evo/fsm/StateMachine.hpp"

namespace evo {

/**
 * @brief Discrete states for modal prompts and confirmation dialogues.
 */
enum class ModalDialogState : int {
    Inactive = 0,
    Prompting = 1,
    Confirmed = 2,
    Cancelled = 3
};

/**
 * @brief Discrete triggers for modal state transitions.
 */
enum class ModalDialogEvent : int {
    Open = 0,
    Confirm = 1,
    Cancel = 2,
    Dismiss = 3
};

class ModalDialogScreen : public StatefulScreen {
public:
    /* The "Stop playback?" prompt over the player (ScreenId::ExitConfirm). */
    ModalDialogScreen();
    ~ModalDialogScreen() override = default;

    // --- IStatefulFeature ---
    IStateMachine* getStateMachine() override { return &m_modalFsm; }
    const IStateMachine* getStateMachine() const override { return &m_modalFsm; }

    ModalDialogState getModalState() const {
        return m_modalFsm.getCurrentState();
    }

    ScreenId getScreenId() const override;
    void onEnter() override;
    void onExit() override;
    bool handleInput(uint32_t pressed, uint32_t held, uint32_t released) override;
    void update(double deltaMs) override;
    void render(uint32_t* framebuffer, int width, int height) override;

private:
    void initModalStateMachine();
    void executeAction(int actionIndex);

    StateMachine<ModalDialogState, ModalDialogEvent> m_modalFsm;
    int m_focusedButton = 1; // Default to the primary action (STOP)
};

} // namespace evo

#endif // EVO_MODAL_DIALOG_SCREEN_HPP
