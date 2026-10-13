#ifndef EVO_TWITCH_CHANNEL_SCREEN_HPP
#define EVO_TWITCH_CHANNEL_SCREEN_HPP

#include "evo/screens/StatefulScreen.hpp"
#include "evo/screens/TwitchScreenData.hpp"

#include <string>
#include <vector>

namespace evo {

/**
 * One channel's page: avatar, name, followers and description (and its next
 * scheduled streams, when it has any), its live stream with how long it has
 * been on, its tags and "Watch live" when it is live, then the Videos and
 * Clips tabs
 * (evo_twitch::channel_page(), channel_videos(), channel_clips()). Videos has
 * the chips Past broadcasts / Highlights / Uploads, Clips has Last 24 hours /
 * 7 days / 30 days / All time (7 days to start with); under them the cards,
 * four to a row, a page of 30 at a time. The kit screen
 * (ui_kit/src/evo_hui_twitch_channel.cpp) draws what render() hands it.
 *
 * Which channel: show() hands the login to the registered screen and goes
 * there, as TwitchCategoryScreen::show does. Search opens it with Cross on a
 * channel, Home and a category with Square on a stream, Following with Cross
 * on an offline channel or Square on any. On the rail it counts as the
 * section it was opened from (ScreenManager::setChannelSection).
 *
 * The cursor starts on Watch live, or on the tabs when the channel is
 * offline. Up and Down go Watch live, the tabs, the chips, the cards; Left
 * and Right move along each (Left at the left edge opens the rail). Moving
 * onto a tab opens it; Cross on a chip shows its list; Cross on a card plays
 * it (evo_twitch_play_video / _clip). A move into the last two rows asks for
 * the next page. Each tab keeps its chip and its cursor while the page is
 * open. Triangle reloads the page and the open tab's list, Circle goes back
 * to where it was opened from.
 */
class TwitchChannelScreen : public StatefulScreen {
public:
    TwitchChannelScreen();
    ~TwitchChannelScreen() override = default;

    /* Opens `login`'s page from another screen. Does nothing if the screen is
     * not registered. */
    static void show(const std::string& login);
    /* The channel the next visit shows, with the cursor back on Watch live,
     * the Videos tab open and the chips back on Past broadcasts and 7 days.
     * show() calls it before going there. */
    void open(const std::string& login);

    ScreenId getScreenId() const override { return ScreenId::TwitchChannel; }
    void onEnter() override;
    bool handleInput(uint32_t pressed, uint32_t held, uint32_t released) override;
    void render(uint32_t* framebuffer, int width, int height) override;

private:
    enum Focus {
        Grid = EVO_HUI_TWITCH_FOCUS_GRID,
        Tabs = EVO_HUI_TWITCH_FOCUS_TABS,
        Filters = EVO_HUI_TWITCH_FOCUS_FILTERS,
        Watch = EVO_HUI_TWITCH_FOCUS_WATCH,
    };
    enum Tab { VideosTab = EVO_HUI_TWITCH_CHANNEL_TAB_VIDEOS, ClipsTab = EVO_HUI_TWITCH_CHANNEL_TAB_CLIPS };

    /* Asks for the page when the slot holds another channel's, is missing,
     * failed or older than a minute; with force, always. This screen is the
     * only one that asks for channel pages, so m_requested says what the
     * slot holds. */
    void refresh(bool force);
    /* The same for the open tab's list (another chip counts as another
     * list), which only goes stale after ten minutes: a refresh starts it
     * from the first page again. This screen is the only one that asks for
     * a channel's videos and clips. */
    void refreshCards(bool force);
    /* The slot holds this channel's page, landed. */
    bool ready() const;
    bool live() const;
    /* The open tab's chip, how many it has, and what its list is asked for as. */
    int filter() const { return m_tab == ClipsTab ? m_clipFilter : m_videoFilter; }
    int filterCount() const;
    std::string cardsKey() const;
    /* The open tab's slot holds the list this page asked for last. */
    bool cardsOurs() const;
    /* Its cards, up to what the kit shows; 0 while it isn't ours. */
    int cardCount() const;
    /* Watch live only while live; the cards only while there are some. */
    void keepFocusValid();
    void openTab(int tab);
    void moveGrid(int dx, int dy);
    /* The next page, when the cursor is in the last two rows. */
    void moreNearEnd();
    /* "Live for 2 h 14 min" and the schedule's "In 5 h": worked out again when
     * the page lands and every ten seconds, not every frame. */
    void updateClockTexts(const evo_twitch::ChannelPage& page);

    std::string m_login;
    std::string m_requested;      /* the login the slot was last asked for */
    int m_focus = Watch;
    int m_tab = VideosTab;
    int m_videoFilter = EVO_HUI_TWITCH_VIDEOS_ARCHIVE;
    int m_clipFilter = EVO_HUI_TWITCH_CLIPS_WEEK;
    int m_filterCursor = 0;
    int m_cursor[2] = {0, 0};     /* per tab */
    std::string m_cardsRequested[2];  /* per tab: the cardsKey() its slot was last asked for */
    std::string m_startingLogin;  /* the stream we asked for, while it starts */
    std::string m_startingId;     /* the video or clip we asked for, while it starts */
    std::string m_uptime;
    std::vector<std::string> m_scheduleWhen;
    unsigned long long m_clockStamp = ~0ull, m_clockTick = ~0ull;
    twitch_screen::RefreshToast m_refreshToast;
    twitch_screen::RefreshToast m_cardsToast[2];
    twitch_screen::PictureKeeper m_pictures;
};

} // namespace evo

#endif // EVO_TWITCH_CHANNEL_SCREEN_HPP
