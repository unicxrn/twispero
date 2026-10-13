#ifndef EVO_TWITCH_SEARCH_SCREEN_HPP
#define EVO_TWITCH_SEARCH_SCREEN_HPP

#include "evo/screens/StatefulScreen.hpp"
#include "evo/screens/TwitchScreenData.hpp"

#include <string>

namespace evo {

/**
 * Search: a field for the query, then the channels and the categories that
 * match it (evo_twitch::search_channels() and search_categories(), up to ten
 * of each). The kit screen (ui_kit/src/evo_hui_twitch_search.cpp) draws what
 * render() hands it.
 *
 * The cursor starts on the field. Cross there opens the keyboard (the native
 * PS5 IME unless Settings picks EVO's own); a submitted query that is not
 * blank is searched for (and kept with the recent searches,
 * evo_twitch_history.h), a blank one clears it. Square on the field clears
 * the query too. With no query the recent searches list under the field:
 * Down goes into them, Cross searches one again, Square removes it. Up/Down move between the field and the two rows, Left/Right
 * within a row (Left at the start of a row, or on the field, opens the rail).
 * Cross opens a channel's page or a category; Triangle runs the search again;
 * Circle in a row goes back to the field, on the field back to where Search
 * was opened from.
 *
 * Keyboard. evo_keyboard_open() returns at once; the text comes back through
 * OnQuerySubmitted, which evo_keyboard calls on the main thread (from the
 * frame loop's input step, evo_keyboard_update() or
 * evo_keyboard_handle_input()) while the keyboard still counts as open, so it
 * only stores the query and asks for it. Cancelling calls nothing. While the
 * keyboard is open Application routes all input to it, so this screen sees
 * none and cannot be left; it is registered for the app's lifetime, so the
 * `this` handed over as userdata stays valid.
 */
class TwitchSearchScreen : public StatefulScreen {
public:
    TwitchSearchScreen();
    ~TwitchSearchScreen() override = default;

    ScreenId getScreenId() const override { return ScreenId::TwitchSearch; }
    void onEnter() override;
    bool handleInput(uint32_t pressed, uint32_t held, uint32_t released) override;
    void render(uint32_t* framebuffer, int width, int height) override;

private:
    enum Row { Field = 0, Channels = 1, Categories = 2, Recent = 3 };

    static void OnQuerySubmitted(const char* text, void* userdata);
    /* A new query from the keyboard (trimmed): searched for when it is not
     * blank. The cursor goes back to the start of both rows. */
    void submit(const std::string& query);
    /* Asks for the query's results when the slots hold another query's, are
     * missing, failed or older than a minute; with force, always. Nothing
     * for an empty query. This screen is the only one that searches, so
     * m_requested says what the slots hold. */
    void refresh(bool force);
    /* The slots hold the results for the query on screen. */
    bool current() const;
    int itemCount(int row) const;
    void keepRowValid();
    void moveRow(int dir);
    void moveCol(int dir);
    void activate();

    std::string m_query;
    std::string m_requested;  /* the query the slots were last asked for */
    int m_row = Field;
    int m_channelCol = 0;
    int m_categoryCol = 0;
    int m_recentCursor = 0;  /* in the recent searches */
    twitch_screen::RefreshToast m_channelsToast, m_categoriesToast;
    twitch_screen::PictureKeeper m_pictures;
};

} // namespace evo

#endif // EVO_TWITCH_SEARCH_SCREEN_HPP
