#ifndef EVO_COMMON_HPP
#define EVO_COMMON_HPP

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <memory>
#include <cstdio>
#include <cstring>
#include <cmath>

namespace evo {

/*
 * The size EVO actually renders at. Resolved once at startup from
 * sceVideoOutGetResolutionStatus (see evo_agc_runtime_init) and fixed for the
 * session - not a constant, because a 1440p panel should be driven at 1440p
 * rather than upscaled from 1080p.
 *
 * This is NOT the UI's unit system. Stylesheets stay authored against the
 * EVO_UI_DESIGN_W x _H canvas in dp (ui/include/evo_metrics.h); the RmlUi
 * contexts get a density-independent pixel ratio of DisplayWidth /
 * EVO_UI_DESIGN_W, so layout and glyph rasterisation happen at the real
 * pixel size while the source geometry never changes.
 */
extern int DisplayWidth;
extern int DisplayHeight;

// Screen Identifiers (compatible with legacy UI router)
//
// The numbers are part of the dev remote (`evo-remote.sh screen <n>`) and the
// devstate names (evo_rmlui_devstate.cpp), so they never move. A removed
// screen leaves its number unused; don't reuse these:
//   1 file browser, 3 resume prompt, 11 profile select, 12 recent files,
//   13 favorites, 15 developer tools, 16 media info, 17 playback finished,
//   18 subtitle picker, 19 changelog,
//   21 text reader, 22 Emby setup, 23 provider screens (Emby browse),
//   24 Settings > Video & Display, 25 Settings > Subtitles,
//   26 Settings > Interface & Storage, 28 surround test, 29 theme select,
//   30 image viewer, 33 Settings > Audio, 34 Settings > Experimental.
enum class ScreenId : int {
    MainMenu = 0,        /* EVO's home; navigateTo() sends it to TwitchHome */
    Player = 2,
    Settings = 10,
    AboutSupport = 14,
    ExitConfirm = 20,
    SettingsSystem = 27,
    AudioTrackPicker = 31,
    SafeToClose = 32,    /* the frame EVO parks on after QUIT (SafeToCloseScreen) */
    /* The Settings sections next to System (stage 6). */
    SettingsTwitch = 35,
    SettingsControls = 36,
    /* The Twitch screens (ps5-twitch stage 3). TwitchHome is where the app
     * opens; MainMenu (EVO's own home) is no longer shown. */
    TwitchHome = 40,
    TwitchBrowse = 41,
    TwitchCategory = 42,
    TwitchSearch = 43,
    TwitchChannel = 44,
    TwitchLogin = 45,    /* stage 5: full screen, opened from Settings */
    TwitchFollowing = 46 /* stage 5: on the rail, after Home */
};

// Playback Quality Profile
enum class PlaybackProfile : int {
    Balanced = 0,
    Performance = 1,
    Compatibility = 2,
    Debug = 3
};

// Video Scaling / Aspect Ratio View Mode
enum class ViewMode : int {
    Fit = 0,
    Fill = 1,
    Stretch = 2
};

// Synthesized UI Sound Effects
enum class SoundEffect : int {
    None = 0,
    Move = 1,
    Confirm = 2,
    Back = 3,
    Toggle = 4
};

// Controller Button Masks (Standard SCE Layout)
namespace PadButtons {
    constexpr uint32_t Options  = 0x0008;
    constexpr uint32_t Up       = 0x0010;
    constexpr uint32_t Right    = 0x0020;
    constexpr uint32_t Down     = 0x0040;
    constexpr uint32_t Left     = 0x0080;
    constexpr uint32_t L2       = 0x0100;
    constexpr uint32_t R2       = 0x0200;
    constexpr uint32_t L1       = 0x0400;
    constexpr uint32_t R1       = 0x0800;
    constexpr uint32_t Triangle = 0x1000;
    constexpr uint32_t Circle   = 0x2000;
    constexpr uint32_t Cross    = 0x4000;
    constexpr uint32_t Square   = 0x8000;
    constexpr uint32_t L3            = 0x0002;
    constexpr uint32_t R3            = 0x0004;
    constexpr uint32_t TouchPad      = 0x00100000; // Physical touchpad click
    constexpr uint32_t TouchPadLeft  = 0x00200000; // Touchpad click/swipe on left half
    constexpr uint32_t TouchPadRight = 0x00400000; // Touchpad click/swipe on right half
}

// Color packing helper for 0xAABBGGRR (BGRA in little-endian uint32_t)
inline constexpr uint32_t MakeColorBgra(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    return (static_cast<uint32_t>(a) << 24) |
           (static_cast<uint32_t>(b) << 16) |
           (static_cast<uint32_t>(g) << 8)  |
           (static_cast<uint32_t>(r));
}

} // namespace evo

#endif // EVO_COMMON_HPP
