#include "twitch/settings.hpp"

#include <nlohmann/json.hpp>

namespace twitch {

namespace {

const char* modeName(ChatMode m) {
  switch (m) {
    case ChatMode::Overlay:
      return "overlay";
    case ChatMode::Off:
      return "off";
    default:
      return "side";
  }
}

const char* sizeName(ChatTextSize s) {
  switch (s) {
    case ChatTextSize::Small:
      return "small";
    case ChatTextSize::Large:
      return "large";
    default:
      return "medium";
  }
}

// A boolean field, if `j` has it as a boolean.
void readBool(const nlohmann::json& j, const char* key, bool& out) {
  if (const auto it = j.find(key); it != j.end() && it->is_boolean()) out = it->get<bool>();
}

}  // namespace

bool isQuality(const std::string& name) noexcept {
  for (const char* q : kQualities)
    if (name == q) return true;
  return false;
}

AppSettings parseSettings(const std::string& json) noexcept {
  AppSettings s;
  try {
    const nlohmann::json j = nlohmann::json::parse(json, nullptr, false);
    if (!j.is_object()) return s;
    if (const auto q = j.find("quality"); q != j.end() && q->is_string()) {
      const std::string v = q->get<std::string>();
      if (isQuality(v)) s.quality = v;
    }
    if (const auto m = j.find("chat_mode"); m != j.end() && m->is_string()) {
      const std::string v = m->get<std::string>();
      if (v == "overlay") s.chatMode = ChatMode::Overlay;
      else if (v == "off") s.chatMode = ChatMode::Off;
    }
    readBool(j, "chat_hint_seen", s.chatHintSeen);
    if (const auto k = j.find("keyboard"); k != j.end() && k->is_string() && k->get<std::string>() == "onscreen")
      s.keyboard = Keyboard::OnScreen;
    readBool(j, "navigation_sounds", s.navigationSounds);
    readBool(j, "lightbar", s.lightbar);
    readBool(j, "debug_overlay", s.debugOverlay);
    if (const auto t = j.find("chat_text_size"); t != j.end() && t->is_string()) {
      const std::string v = t->get<std::string>();
      if (v == "small") s.chatTextSize = ChatTextSize::Small;
      else if (v == "large") s.chatTextSize = ChatTextSize::Large;
    }
    readBool(j, "third_party_emotes", s.thirdPartyEmotes);
    readBool(j, "live_notifications", s.liveNotifications);
  } catch (...) {
    return AppSettings{};
  }
  return s;
}

std::string serializeSettings(const AppSettings& s) {
  nlohmann::json j;
  j["quality"] = s.quality;
  j["chat_mode"] = modeName(s.chatMode);
  j["chat_hint_seen"] = s.chatHintSeen;
  j["keyboard"] = s.keyboard == Keyboard::OnScreen ? "onscreen" : "system";
  j["navigation_sounds"] = s.navigationSounds;
  j["lightbar"] = s.lightbar;
  j["debug_overlay"] = s.debugOverlay;
  j["chat_text_size"] = sizeName(s.chatTextSize);
  j["third_party_emotes"] = s.thirdPartyEmotes;
  j["live_notifications"] = s.liveNotifications;
  return j.dump() + "\n";
}

ChatMode nextChatMode(ChatMode m) {
  switch (m) {
    case ChatMode::Side:
      return ChatMode::Overlay;
    case ChatMode::Overlay:
      return ChatMode::Off;
    default:
      return ChatMode::Side;
  }
}

}  // namespace twitch
