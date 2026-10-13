#ifndef EVO_TWITCH_BROWSE_SCREEN_HPP
#define EVO_TWITCH_BROWSE_SCREEN_HPP

#include "evo/screens/StatefulScreen.hpp"
#include "evo/screens/TwitchScreenData.hpp"

namespace evo {

/**
 * Browse: the top categories as a grid of box art, six to a row. Shares its
 * list with Home's "Top categories" (evo_twitch::home_categories()); the kit
 * screen (ui_kit/src/evo_hui_twitch_browse.cpp) draws what render() hands it.
 *
 * The D-pad moves the cursor on the grid (Left at the start of a row opens the
 * rail), Cross opens the category (TwitchCategoryScreen), Triangle reloads the
 * list, Circle goes back (normally to Home).
 */
class TwitchBrowseScreen : public StatefulScreen {
public:
    TwitchBrowseScreen();
    ~TwitchBrowseScreen() override = default;

    ScreenId getScreenId() const override { return ScreenId::TwitchBrowse; }
    void onEnter() override;
    bool handleInput(uint32_t pressed, uint32_t held, uint32_t released) override;
    void render(uint32_t* framebuffer, int width, int height) override;

private:
    /* Asks for the list when it is missing, failed or older than a minute;
     * with force, always. */
    void refresh(bool force);
    int count() const;
    void move(int dx, int dy);
    void activate();

    int m_cursor = 0;
    twitch_screen::RefreshToast m_refreshToast;
    twitch_screen::PictureKeeper m_pictures;
};

} // namespace evo

#endif // EVO_TWITCH_BROWSE_SCREEN_HPP
