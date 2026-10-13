#ifndef EVO_TWITCH_CATEGORY_SCREEN_HPP
#define EVO_TWITCH_CATEGORY_SCREEN_HPP

#include "evo/screens/StatefulScreen.hpp"
#include "evo/screens/TwitchScreenData.hpp"

#include <string>

namespace evo {

/**
 * One category: its box art, name and viewers, then two tabs: Live, its live
 * streams four to a row (evo_twitch::category_streams()), and Clips, its
 * clips from the last 24 hours, 7 days, 30 days or all time (7 days to start
 * with; evo_twitch::category_clips()). The kit screen
 * (ui_kit/src/evo_hui_twitch_browse.cpp) draws what render() hands it. On the
 * rail it counts as Browse.
 *
 * Which category: show() hands the name to the registered screen and goes
 * there, the way EVO's browser hands a path to the image viewer. The screen
 * keeps it while you watch a stream from it and come back.
 *
 * The cursor starts on the first live stream. The D-pad moves it on the grid
 * (Left at the start of a row opens the rail); Up from the first row goes to
 * the clips' chips and the tabs, where moving onto a tab opens it and Cross
 * on a chip shows its clips. Cross plays the stream or the clip, Square opens
 * its channel page, Triangle reloads the open tab's list, Circle goes back
 * to where it was opened from (Browse, Home or Search).
 */
class TwitchCategoryScreen : public StatefulScreen {
public:
    TwitchCategoryScreen();
    ~TwitchCategoryScreen() override = default;

    /* Opens `name` from another screen: viewers is -1 when not known, the box
     * art URL may be empty. Does nothing if the screen is not registered. */
    static void show(const std::string& name, int viewers, const std::string& boxArtUrl);
    /* The category the next visit shows, with the cursor back on its first
     * stream, the Live tab open and the clips' chip on 7 days. show() calls
     * it before going there. */
    void open(const std::string& name, int viewers, const std::string& boxArtUrl);

    ScreenId getScreenId() const override { return ScreenId::TwitchCategory; }
    void onEnter() override;
    bool handleInput(uint32_t pressed, uint32_t held, uint32_t released) override;
    void render(uint32_t* framebuffer, int width, int height) override;

private:
    enum Focus {
        Grid = EVO_HUI_TWITCH_FOCUS_GRID,
        Tabs = EVO_HUI_TWITCH_FOCUS_TABS,
        Filters = EVO_HUI_TWITCH_FOCUS_FILTERS,
    };
    enum Tab { LiveTab = EVO_HUI_TWITCH_CATEGORY_TAB_LIVE, ClipsTab = EVO_HUI_TWITCH_CATEGORY_TAB_CLIPS };

    /* Asks for the streams when the list is for another category, missing,
     * failed or older than a minute; with force, always. This screen is the
     * only one that asks for category streams, so m_requested says what the
     * slot holds. */
    void refresh(bool force);
    /* The same for the clips (another chip counts as another list). */
    void refreshClips(bool force);
    std::string clipsKey() const;
    bool clipsOurs() const;
    int count() const;      /* the live streams shown */
    int clipCount() const;  /* the clips shown, 0 while the slot isn't ours */
    /* The open tab's: its cards, its cursor, and whether its list is still loading. */
    int tabCount() const { return m_tab == ClipsTab ? clipCount() : count(); }
    int& tabCursor() { return m_tab == ClipsTab ? m_clipCursor : m_cursor; }
    bool tabLoading() const;
    /* The channel of the stream or clip under the cursor, or "". */
    std::string focusedLogin() const;
    void move(int dx, int dy);
    /* The cards only while there are some (the first ones may still come). */
    void keepFocusValid();

    std::string m_name;
    std::string m_boxArtUrl;
    int m_viewers = -1;
    std::string m_requested;      /* the name the slot was last asked for */
    int m_cursor = 0;
    int m_tab = LiveTab;
    int m_focus = Grid;
    int m_clipFilter = EVO_HUI_TWITCH_CLIPS_WEEK;
    int m_filterCursor = EVO_HUI_TWITCH_CLIPS_WEEK;
    int m_clipCursor = 0;
    std::string m_clipsRequested; /* the clipsKey() the clips' slot was last asked for */
    std::string m_startingLogin;  /* the stream we asked for, while it starts */
    std::string m_startingId;     /* the clip we asked for, while it starts */
    twitch_screen::RefreshToast m_refreshToast, m_clipsToast;
    twitch_screen::PictureKeeper m_pictures;
};

} // namespace evo

#endif // EVO_TWITCH_CATEGORY_SCREEN_HPP
