#ifndef EVO_TWITCH_HOME_SCREEN_HPP
#define EVO_TWITCH_HOME_SCREEN_HPP

#include "evo/screens/StatefulScreen.hpp"
#include "evo/screens/TwitchScreenData.hpp"

#include "evo_twitch_auth.h"

#include <string>
#include <vector>

namespace evo {

/**
 * The Twitch home: the most-watched stream as the hero, "Popular now" (the
 * next ones) and "Top categories". While a user is logged in and some of the
 * channels they follow are live, "Following · live now" sits between the
 * hero and Popular now (evo_twitch::followed_live(), asked for whenever the
 * login changes); otherwise Home is as it was. "Recently watched", the
 * channels watched live (evo_twitch_history.h), comes next whenever there are
 * any, logged in or not: a channel shows as live when it is in Following or
 * Popular now's lists (nothing more is asked for it); Cross watches a live
 * one and opens an offline one's page. "Continue watching", the videos with
 * saved progress (evo_twitch_progress.h), comes after it whenever there are
 * any; Cross picks one up where it was left. Owns the cursor and asks the
 * data service (twitch/include/evo_twitch_data.h) for the lists; the kit
 * screen (ui_kit/src/evo_hui_twitch_home.cpp) draws what render() hands it.
 *
 * Up/Down move between the hero and the rows, Left/Right within a row (Left
 * at the start of a row opens the rail), Cross plays a stream or a video or
 * opens a category, Square opens a stream's, a channel's or a video's page,
 * Triangle reloads every list, Circle in a row goes back to the hero.
 */
class TwitchHomeScreen : public StatefulScreen {
public:
    TwitchHomeScreen();
    ~TwitchHomeScreen() override = default;

    ScreenId getScreenId() const override { return ScreenId::TwitchHome; }
    void onEnter() override;
    void update(double deltaMs) override;
    bool handleInput(uint32_t pressed, uint32_t held, uint32_t released) override;
    void render(uint32_t* framebuffer, int width, int height) override;

private:
    /* The kit's EVO_HUI_TWITCH_ROW_* ids; kRowOrder is the order on screen. */
    enum Row { Hero = 0, Followed = 1, Streams = 2, Categories = 3, Continue = 4, Recent = 5 };

    /* Notices a login, a logout or another user: a new user's followed live
     * channels are asked for. */
    void followLogin();
    /* Asks for the lists that are missing, failed or older than a minute;
     * with force, for all of them. */
    void refresh(bool force);
    int itemCount(int row) const;
    /* After a refresh: a row that has gone empty hands the cursor to the
     * nearest row that has something (Following's to Popular now first). */
    void keepRowValid();
    void moveRow(int dir);
    void moveCol(int dir);
    /* The stream under the cursor (the hero, Following or Popular now), or
     * the channel of the video under it, or "". */
    std::string focusedLogin() const;
    void activate();
    /* The cards of Continue watching, from the saved progress. */
    void fillContinue(evo_hui_twitch_home_params_t& p);
    /* Recently watched, from the watch history, with who of it is live. */
    void fillRecent(evo_hui_twitch_home_params_t& p);
    /* The viewers of each channel in the history that Following or Popular
     * now has live, -1 for the rest; worked out again when any of the three
     * lists changes. */
    const std::vector<int>& recentViewers();

    int m_row = Hero;
    int m_followedCol = 0;
    int m_streamCol = 0;    /* in Popular now (stream 1 + col) */
    int m_categoryCol = 0;
    int m_continueCol = 0;
    int m_recentCol = 0;
    std::string m_userId;   /* whose follows the row shows, "" while logged out */
    std::string m_startingLogin;  /* the stream we asked for, while it starts */
    std::string m_startingId;     /* the video we asked for, while it starts */
    /* Continue watching's chips ("42 min left"), built again when the
     * progress changes (evo_twitch::progress_revision()). */
    std::vector<std::string> m_continueLines;
    unsigned m_continueRevision = ~0u;
    std::vector<int> m_recentViewers;
    unsigned m_recentRevision = ~0u;
    unsigned long long m_recentFollowedStamp = ~0ull, m_recentStreamsStamp = ~0ull;
    std::string m_recentUser;
    twitch_screen::RefreshToast m_streamsToast, m_categoriesToast, m_followedToast;
    twitch_screen::OutageToasts m_outage;
    twitch_screen::PictureKeeper m_pictures;
    /* Which cards of each row the kit draws: pictures only for those. */
    twitch_screen::RowWindow m_followedWindow{EVO_HUI_TWITCH_HOME_STREAM_W, EVO_HUI_TWITCH_HOME_STREAM_MARGIN};
    twitch_screen::RowWindow m_continueWindow{EVO_HUI_TWITCH_HOME_STREAM_W, EVO_HUI_TWITCH_HOME_STREAM_MARGIN};
    twitch_screen::RowWindow m_recentWindow{EVO_HUI_TWITCH_HOME_RECENT_W, EVO_HUI_TWITCH_HOME_RECENT_MARGIN};
    twitch_screen::RowWindow m_streamWindow{EVO_HUI_TWITCH_HOME_STREAM_W, EVO_HUI_TWITCH_HOME_STREAM_MARGIN};
    twitch_screen::RowWindow m_categoryWindow{EVO_HUI_TWITCH_HOME_CATEGORY_W, EVO_HUI_TWITCH_HOME_CATEGORY_MARGIN};
};

} // namespace evo

#endif // EVO_TWITCH_HOME_SCREEN_HPP
