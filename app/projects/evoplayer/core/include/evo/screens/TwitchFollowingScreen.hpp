#ifndef EVO_TWITCH_FOLLOWING_SCREEN_HPP
#define EVO_TWITCH_FOLLOWING_SCREEN_HPP

#include "evo/screens/StatefulScreen.hpp"
#include "evo/screens/TwitchScreenData.hpp"

#include "evo_twitch_auth.h"

#include <string>
#include <vector>

namespace evo {

/**
 * The channels the logged-in user follows, on the rail after Home: "Live
 * now", the live ones as stream cards four to a row (evo_twitch::
 * followed_live()), then "Offline", every other followed channel as an
 * avatar with its name, seven to a row (evo_twitch::followed_channels(),
 * less the live ones). The kit screen (ui_kit/src/
 * evo_hui_twitch_following.cpp) draws what render() hands it.
 *
 * It follows the login service (evo_twitch_auth.h) every frame: once a user
 * is logged in it asks for their follows, and when they log out the data
 * service empties the lists. Until the saved login has been read the page
 * shows skeletons; logged out, it says how to log in and Cross opens the
 * login screen, which comes back here when it is done.
 *
 * Logged in, one cursor runs through both grids: the D-pad moves it (Down
 * from the last row of live cards goes on to the avatars, Left at the start
 * of a row opens the rail), Cross plays a live channel and opens an offline
 * one's page, Square opens the page of either, Triangle reloads both lists,
 * Circle goes back to Home. When the login lacks the follows scope (only a
 * new login grants it), Triangle can't help: the page says to log in again
 * from Settings, and Cross goes there.
 */
class TwitchFollowingScreen : public StatefulScreen {
public:
    TwitchFollowingScreen();
    ~TwitchFollowingScreen() override = default;

    ScreenId getScreenId() const override { return ScreenId::TwitchFollowing; }
    void onEnter() override;
    void update(double deltaMs) override;
    bool handleInput(uint32_t pressed, uint32_t held, uint32_t released) override;
    void render(uint32_t* framebuffer, int width, int height) override;

private:
    /* Notices a login, a logout or another user since the last frame: a new
     * user's follows are asked for and the cursor goes back to the top. */
    void followLogin();
    /* Asks for the lists that are missing, failed or older than a minute;
     * with force, for both. Nothing while logged out. */
    void refresh(bool force);
    /* The cards as the kit gets them: the live ones, then the offline ones
     * (indices into followed_channels()). */
    int liveCount() const;
    int offlineCount() const { return static_cast<int>(m_offline.size()); }
    /* Works out m_offline again when either list has changed, and keeps the
     * cursor on a card. Before anything reads the cursor: a refresh can land
     * shorter lists in any frame. */
    void updateOffline();
    void keepCursorValid();
    void move(int dx, int dy);
    /* The channel under the cursor: its login and whether it is live. */
    bool focused(std::string* login, bool* live) const;

    std::string m_userId;  /* whose follows the lists are, "" while logged out */
    int m_cursor = 0;      /* live cards first, then the offline ones */
    std::vector<int> m_offline;
    /* What m_offline was worked out from. */
    unsigned long long m_liveStamp = ~0ull, m_channelsStamp = ~0ull;
    std::size_t m_liveSize = ~std::size_t(0), m_channelsSize = ~std::size_t(0);
    std::string m_startingLogin;  /* the stream we asked for, while it starts */
    twitch_screen::RefreshToast m_liveToast, m_channelsToast;
    twitch_screen::OutageToasts m_outage;
    twitch_screen::PictureKeeper m_pictures;
};

} // namespace evo

#endif // EVO_TWITCH_FOLLOWING_SCREEN_HPP
