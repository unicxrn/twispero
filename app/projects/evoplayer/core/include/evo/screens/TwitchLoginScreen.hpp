#ifndef EVO_TWITCH_LOGIN_SCREEN_HPP
#define EVO_TWITCH_LOGIN_SCREEN_HPP

#include "evo/screens/StatefulScreen.hpp"

#include "evo_twitch_auth.h"

#include <string>

namespace evo {

/**
 * Logging in to Twitch with the device code: the code in large letters and a
 * QR for twitch.tv/activate, which the user approves on their phone. The
 * login service (twitch/include/evo_twitch_auth.h) does the work; this screen
 * starts it, shows it and leaves when it is done. The kit screen
 * (ui_kit/src/evo_hui_twitch_login.cpp) draws what render() hands it.
 *
 * Settings opens it with navigateTo(ScreenId::TwitchLogin) when logged out.
 * Full screen, with no rail.
 *
 * Entering asks for a code, once the saved login has been read (until then
 * the page says "Checking your Twitch login"). While the code is up, nothing
 * but Circle does anything: Circle drops the login and goes back. A code
 * that runs out is replaced with a new one by itself, as the page says ("The
 * code changes in N min"), up to kMaxRenewals times a visit: after that the
 * failure stays up for an unattended screen. After a failure Cross tries
 * again. Once logged in, "Logged in as <name>" shows for two seconds, then
 * the screen goes back to where it was opened from. Leaving any other way
 * drops a login on its way too. While the kit is down (the plain fallback
 * page shows instead) nothing is asked of Twitch.
 */
class TwitchLoginScreen : public StatefulScreen {
public:
    TwitchLoginScreen();
    ~TwitchLoginScreen() override = default;

    ScreenId getScreenId() const override { return ScreenId::TwitchLogin; }
    void onEnter() override;
    void onExit() override;
    bool handleInput(uint32_t pressed, uint32_t held, uint32_t released) override;
    void update(double deltaMs) override;
    void render(uint32_t* framebuffer, int width, int height) override;

private:
    /* Follows the login service, on entering and then every frame: asks for
     * a code, replaces one that ran out, leaves once logged in. */
    void follow();
    /* Back to the screen it was opened from (Settings if there is none). */
    void leave();

    evo_twitch::AuthState m_lastState = evo_twitch::AuthState::Unknown;
    long long m_codeEndsMs = 0;   /* when the code last shown runs out, data_now_ms() clock */
    int m_renewals = 0;           /* codes replaced by themselves this visit */
    unsigned long long m_doneAtMs = 0;  /* when the login went through here, 0 before */
};

} // namespace evo

#endif // EVO_TWITCH_LOGIN_SCREEN_HPP
