#ifndef EVO_SETTINGS_SERVICE_HPP
#define EVO_SETTINGS_SERVICE_HPP

#include "evo/interfaces/ISettingsService.hpp"

namespace evo {

class SettingsService : public ISettingsService {
public:
    SettingsService() = default;
    ~SettingsService() override = default;

    bool loadSettings() override;
    bool saveSettings() override;

    bool isSoundFeedbackEnabled() const override;
    void setSoundFeedbackEnabled(bool enabled) override;

    bool isLightbarFeedbackEnabled() const override;
    void setLightbarFeedbackEnabled(bool enabled) override;

    int getKeyboardType() const override;
    void setKeyboardType(int type) override;

    bool isDebugOverlayEnabled() const override;
    void setDebugOverlayEnabled(bool enabled) override;

private:
    void syncThemeToRmlUi();

    /* evo_player_settings.cfg, EVO's own settings file, has been deleted
     * (or tried) once the sandbox was open. */
    bool m_oldFileRemoved = false;
};

} // namespace evo

#endif // EVO_SETTINGS_SERVICE_HPP
