#pragma once
#include <string>

namespace twitch {

enum class ChatMode { Side, Overlay, Off };

// Which keyboard types text: the PS5's own, or the one the app draws.
enum class Keyboard { System, OnScreen };

// How big chat's text is, in the order Settings lists them.
enum class ChatTextSize { Small, Medium, Large };

// The default-quality choices, in the order Settings lists them. "auto" takes
// the best variant the stream has; the others are Twitch's variant names
// ("audio_only" is sound only).
inline constexpr const char* kQualities[] = {"auto", "source", "1080p60", "720p60", "720p",
                                             "480p", "360p",   "160p",    "audio_only"};
inline constexpr int kQualityCount = static_cast<int>(sizeof kQualities / sizeof kQualities[0]);

// True for a name in kQualities (exact match).
bool isQuality(const std::string& name) noexcept;

struct AppSettings {
  std::string quality = "auto";  // always one of kQualities
  ChatMode chatMode = ChatMode::Side;
  bool chatHintSeen = false;  // the "R1 changes the chat" hint has been shown once
  Keyboard keyboard = Keyboard::System;
  bool navigationSounds = true;
  bool lightbar = true;  // the controller's light follows the app's colour
  bool debugOverlay = false;
  ChatTextSize chatTextSize = ChatTextSize::Medium;
  bool thirdPartyEmotes = true;  // 7TV, BetterTTV and FrankerFaceZ emotes in chat
  bool liveNotifications = true;  // a note when a followed channel goes live
  bool homePreview = true;        // Home's hero plays its stream, muted
  // Browse's stream language (kBrowseLanguages). "" is every language.
  std::string browseLanguage;
};

// Never throws: a missing, malformed or unknown field keeps its default.
AppSettings parseSettings(const std::string& json) noexcept;
std::string serializeSettings(const AppSettings& settings);

ChatMode nextChatMode(ChatMode mode);  // side -> overlay -> off -> side

}  // namespace twitch
