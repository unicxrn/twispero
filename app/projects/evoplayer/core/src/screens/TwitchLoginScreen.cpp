#include "evo/screens/TwitchLoginScreen.hpp"
#include "evo/screens/TwitchScreenData.hpp"
#include "evo/Application.hpp"
#include "evo_rmlui_bridge.h"

#include <cstring>

namespace evo {

namespace {

using evo_twitch::AuthState;

/* How long "Logged in as <name>" stays before the screen goes back. */
constexpr unsigned long long kDoneMs = 2000;

/* A failure this close to the code's end is the code running out: Twitch
 * counts from when it made the code, a little before we had it, and a poll
 * that fails near the end says so in its own words. */
constexpr long long kRenewGraceMs = 30000;
/* Codes replaced by themselves in one visit: a screen left alone doesn't
 * ask Twitch for codes all day. Cross starts the count again. */
constexpr int kMaxRenewals = 3;

}  // namespace

TwitchLoginScreen::TwitchLoginScreen()
    : StatefulScreen("TwitchLoginScreen") {
}

void TwitchLoginScreen::onEnter() {
    StatefulScreen::onEnter();
    m_doneAtMs = 0;
    m_codeEndsMs = 0;
    m_renewals = 0;
    /* A failure left from another visit: start afresh. LoggedOut is
     * follow()'s, and a login already on its way just shows. */
    if (!twitch_screen::fallbackActive() && evo_twitch::auth_view().state == AuthState::Error)
        evo_twitch::auth_begin_login();
    m_lastState = evo_twitch::auth_view().state;
    follow();
}

void TwitchLoginScreen::onExit() {
    StatefulScreen::onExit();
    /* However the screen is left, a login it started doesn't run on unseen. */
    const AuthState s = evo_twitch::auth_view().state;
    if (s == AuthState::Starting || s == AuthState::WaitingForUser || s == AuthState::Error)
        evo_twitch::auth_cancel_login();
    m_doneAtMs = 0;
}

void TwitchLoginScreen::update(double deltaMs) {
    StatefulScreen::update(deltaMs);
    follow();
}

void TwitchLoginScreen::follow() {
    /* The fallback page shows instead of this one: no code nobody can see. */
    if (twitch_screen::fallbackActive()) return;
    const evo_twitch::AuthView& v = evo_twitch::auth_view();
    const unsigned long long now = evo_twitch::data_now_ms();
    switch (v.state) {
        case AuthState::LoggedOut:
            /* On the way in, and when the saved login (Unknown until it has
             * been read) turned out not to be there. */
            evo_twitch::auth_begin_login();
            break;
        case AuthState::Error:
            /* The code on screen ran out unused: the page said it would
             * change, so a new one comes without a press. Any other failure
             * waits for Cross, and so does this one once kMaxRenewals codes
             * went unused. */
            if (m_lastState == AuthState::WaitingForUser && m_codeEndsMs > 0 &&
                static_cast<long long>(now) + kRenewGraceMs >= m_codeEndsMs && m_renewals < kMaxRenewals) {
                ++m_renewals;
                evo_twitch::auth_begin_login();
            }
            break;
        case AuthState::WaitingForUser:
            m_codeEndsMs = v.codeExpiresAtMs;
            break;
        case AuthState::LoggedIn:
            if (m_doneAtMs == 0) {
                m_doneAtMs = now ? now : 1;
                evo_feedback(EVO_FB_CONFIRM);
            } else if (now - m_doneAtMs >= kDoneMs) {
                leave();
                return;
            }
            break;
        default:
            break;
    }
    m_lastState = evo_twitch::auth_view().state;
}

void TwitchLoginScreen::leave() {
    if (auto sm = Application::getInstance().getScreenManager())
        sm->navigateBack(ScreenId::Settings);
}

bool TwitchLoginScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released) {
    (void)held;
    (void)released;
    if (pressed & PadButtons::Circle) {
        /* onExit() drops the login on its way. */
        evo_feedback(EVO_FB_CANCEL);
        leave();
        return true;
    }
    if (twitch_screen::fallbackActive()) return twitch_screen::fallbackInput(pressed);

    if (pressed & PadButtons::Cross) {
        if (evo_twitch::auth_view().state == AuthState::Error) {
            m_renewals = 0;
            evo_twitch::auth_begin_login();
            /* Still Error: logging in can't work in this build. */
            evo_feedback(evo_twitch::auth_view().state == AuthState::Error ? EVO_FB_ERROR : EVO_FB_CONFIRM);
            m_lastState = evo_twitch::auth_view().state;
        }
        return true;
    }
    return false;
}

void TwitchLoginScreen::render(uint32_t* framebuffer, int width, int height) {
    if (twitch_screen::fallbackActive()) {
        twitch_screen::renderFallback(framebuffer, width, height);
        return;
    }
    const evo_twitch::AuthView& v = evo_twitch::auth_view();

    evo_hui_twitch_login_params_t p;
    std::memset(&p, 0, sizeof(p));
    p.seconds_left = -1;
    switch (v.state) {
        case AuthState::WaitingForUser: {
            p.state = EVO_HUI_TWITCH_LOGIN_WAITING;
            /* Plain, no hyphen: the kit draws the two groups of four with a
             * gap, not a character someone reading the box by eye would
             * also type at twitch.tv/activate. */
            p.code = v.userCode.c_str();
            const long long left = v.codeExpiresAtMs - static_cast<long long>(evo_twitch::data_now_ms());
            p.seconds_left = left > 0 ? static_cast<int>(left / 1000) : 0;
            const std::size_t modules = static_cast<std::size_t>(v.qr.size > 0 ? v.qr.size : 0);
            if (modules > 0 && v.qr.dark.size() == modules * modules) {
                p.qr = v.qr.dark.data();
                p.qr_size = v.qr.size;
            }
            break;
        }
        case AuthState::LoggedIn:
            p.state = EVO_HUI_TWITCH_LOGIN_DONE;
            p.name = v.displayName.c_str();
            p.login = v.login.c_str();
            break;
        case AuthState::Error:
            p.state = EVO_HUI_TWITCH_LOGIN_ERROR;
            p.error = v.error.c_str();
            break;
        case AuthState::Unknown:
            /* The saved login isn't read yet: no code has been asked for. */
            p.state = EVO_HUI_TWITCH_LOGIN_CHECKING;
            break;
        default:
            /* Starting, and the moment of LoggedOut before follow() asks
             * for a code. */
            p.state = EVO_HUI_TWITCH_LOGIN_STARTING;
            break;
    }

#if defined(EVO_AGC_DEVICE)
    evo_hui_update_twitch_login(&p);
    evo_rmlui_render_kit_screen(evo_hui_render_twitch_login, width, height);
#else
    (void)p;
    (void)width;
    (void)height;
#endif
}

} // namespace evo
