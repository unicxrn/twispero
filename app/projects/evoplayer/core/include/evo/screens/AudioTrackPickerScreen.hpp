#ifndef EVO_AUDIO_TRACK_PICKER_SCREEN_HPP
#define EVO_AUDIO_TRACK_PICKER_SCREEN_HPP

#include "evo/screens/StatefulScreen.hpp"

#include <string>
#include <vector>

namespace evo {

/*
 * The quality picker (R2 in the player). It keeps EVO's audio track picker's
 * name and screen id (ScreenId::AudioTrackPicker, 31) so the dev remote's
 * numbers stay put; a Twitch stream has one AAC track, so the audio part is
 * gone.
 *
 * One row per quality the Twitch stream offers (evo_twitch_qualities()), best
 * first and Audio only last, with a tick on the one playing. Choosing another
 * hands it to the launcher (evo_twitch_switch_quality()), which looks the
 * stream up again while the old quality keeps playing, then restarts the
 * player in the new one. Choosing the one playing just closes the picker.
 */
class AudioTrackPickerScreen : public StatefulScreen {
public:
    AudioTrackPickerScreen();
    ~AudioTrackPickerScreen() override = default;

    ScreenId getScreenId() const override { return ScreenId::AudioTrackPicker; }
    void onEnter() override;
    void onExit() override;
    bool handleInput(uint32_t pressed, uint32_t held, uint32_t released) override;
    void update(double deltaMs) override;
    void render(uint32_t* framebuffer, int width, int height) override;

private:
    struct Row {
        std::string name;    /* Twitch's name, for evo_twitch_switch_quality() */
        std::string label;   /* "1080p60 (source)", "720p60", "Audio only" */
        std::string detail;  /* "1920 x 1080, 60 fps" */
        bool audioOnly = false;
    };

    void refreshRows();
    void navigate(int delta);
    void activateSelection();
    void close();

    std::vector<Row> m_rows;
    int m_selectedIndex = 0;
    int m_currentIndex = -1;   /* the quality playing, -1 if unknown */
    int m_scrollOffset = 0;
};

} // namespace evo

#endif // EVO_AUDIO_TRACK_PICKER_SCREEN_HPP
