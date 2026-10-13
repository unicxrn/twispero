#pragma once
// evo_twitch_chat_mode.h - how chat is drawn this frame, for the two places
// that must agree on it: Application puts the video in its box
// (evo_agc_runtime_set_video_rect) before the blit, and PlayerScreen draws
// chat and fits the OSD to the same box. Both ask here, in the same frame,
// so the picture and the panel beside it never disagree. Main thread only.

#include <string>

#include "evo_twitch_chat_layout.h"
#include "evo_twitch_settings.h"

namespace evo_twitch {

// A playback source is a Twitch stream: the launcher names the provider
// "twitch" and puts the login in the item id (evo_twitch_launcher.cpp). Only
// a live stream has chat, so only it is one of these.
inline bool is_twitch_source(const std::string& provider, const std::string& item_id) noexcept {
  return provider == "twitch" && !item_id.empty();
}

// A Twitch past broadcast, highlight, upload or clip: the launcher names the
// provider "twitch-vod" or "twitch-clip" (the id or slug in the item id). No
// chat, the whole screen, a timeline.
inline bool is_twitch_video_source(const std::string& provider, const std::string& item_id) noexcept {
  return (provider == "twitch-vod" || provider == "twitch-clip") && !item_id.empty();
}

// Off while no Twitch stream plays; otherwise the mode in the settings.
inline twitch::ChatMode chat_mode_drawn(bool twitchPlaying) noexcept {
  return twitchPlaying ? settings().chatMode : twitch::ChatMode::Off;
}

// Where the video goes in that mode, as fractions of the screen: the box left
// of the chat panel in side mode, the whole screen otherwise.
inline void chat_video_box(twitch::ChatMode mode, float* x, float* y, float* w, float* h) noexcept {
  if (mode == twitch::ChatMode::Side) {
    evo_chat_side_box(x, y, w, h);
  } else {
    *x = 0.0f;
    *y = 0.0f;
    *w = 1.0f;
    *h = 1.0f;
  }
}

}  // namespace evo_twitch
