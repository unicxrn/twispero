#ifndef EVO_I_SETTINGS_SERVICE_HPP
#define EVO_I_SETTINGS_SERVICE_HPP

#include "evo/Common.hpp"

namespace evo {

/**
 * @brief The app's settings that EVO's own code applies: the keyboard, the
 * navigation sounds, the controller light and the debug overlay.
 *
 * They live in the app's one settings file, twitch_settings.json, with the
 * Twitch settings (default quality, chat), which the Twitch code reads
 * straight from evo_twitch_settings.h. saveSettings() writes all of them.
 */
class ISettingsService {
public:
    virtual ~ISettingsService() = default;

    /* Applies the saved settings. Called at boot and again after a late
     * unjail, when the settings file can first be read. */
    virtual bool loadSettings() = 0;
    virtual bool saveSettings() = 0;

    virtual bool isSoundFeedbackEnabled() const = 0;
    virtual void setSoundFeedbackEnabled(bool enabled) = 0;

    virtual bool isLightbarFeedbackEnabled() const = 0;
    virtual void setLightbarFeedbackEnabled(bool enabled) = 0;

    /* EVO_KEYBOARD_TYPE_NATIVE (the PS5's keyboard) or EVO_KEYBOARD_TYPE_VIRTUAL
     * (the app's on-screen one). */
    virtual int getKeyboardType() const = 0;
    virtual void setKeyboardType(int type) = 0;

    virtual bool isDebugOverlayEnabled() const = 0;
    virtual void setDebugOverlayEnabled(bool enabled) = 0;
};

} // namespace evo

#endif // EVO_I_SETTINGS_SERVICE_HPP
