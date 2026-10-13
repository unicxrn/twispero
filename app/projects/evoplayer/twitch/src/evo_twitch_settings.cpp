// evo_twitch_settings.cpp - see evo_twitch_settings.h.
//
// State lives in file-scope static storage, placement-new'd on first use and
// never destroyed (same pattern as the State slot in evo_twitch_data.cpp:
// alignas storage + a pointer, no function-local statics). g_loaded is a
// plain bool: the file read is tried at most once, on the first settings()
// or set_*() call after the sandbox opens.

#include "evo_twitch_settings.h"

#include <stdio.h>

#include <new>
#include <string>

#include "evo_boot_log.h"
#include "evo_data_path.h"
#include "evo_jailbreak.h"

namespace evo_twitch {

namespace {

constexpr const char* kSettingsFile = "twitch_settings.json";
constexpr const char* kSettingsTmpFile = "twitch_settings.json.tmp";
constexpr size_t kMaxSettingsBytes = 64 * 1024;

alignas(twitch::AppSettings) unsigned char g_storage[sizeof(twitch::AppSettings)];
twitch::AppSettings* g_settings = nullptr;
bool g_loaded = false;  // the file has been read (or tried) once

twitch::AppSettings& stored() noexcept {
  if (!g_settings) g_settings = new (g_storage) twitch::AppSettings();
  return *g_settings;
}

// Reads `path` into `out`, capped at kMaxSettingsBytes. Leaves `out` empty on
// a missing file, a read error or an exception; parseSettings treats that the
// same as a broken file (defaults).
void readFile(const char* path, std::string& out) noexcept {
  out.clear();
  try {
    FILE* f = fopen(path, "rb");
    if (!f) return;
    char buf[4096];
    size_t n;
    while (out.size() < kMaxSettingsBytes && (n = fread(buf, 1, sizeof buf, f)) > 0) {
      size_t room = kMaxSettingsBytes - out.size();
      out.append(buf, n < room ? n : room);
    }
    fclose(f);
  } catch (...) {
    out.clear();
  }
}

// The settings, read from the file first if that is due, so a change made
// through a setter is never overwritten by the read that follows it.
twitch::AppSettings& loaded() noexcept {
  try {
    if (!g_loaded && evo_jailbreak_is_open()) {
      std::string json;
      readFile(evo_data_path(kSettingsFile), json);
      stored() = twitch::parseSettings(json);
      g_loaded = true;
    }
  } catch (...) {
    g_loaded = true;
  }
  return stored();
}

}  // namespace

const twitch::AppSettings& settings() noexcept { return loaded(); }

void set_quality(const char* name) noexcept {
  try {
    const std::string q = name ? name : "";
    loaded().quality = twitch::isQuality(q) ? q : "auto";
  } catch (...) {
    // Out of memory: the setting stays as it was.
  }
}

void set_chat_mode(twitch::ChatMode mode) noexcept { loaded().chatMode = mode; }
void set_chat_hint_seen(bool seen) noexcept { loaded().chatHintSeen = seen; }
void set_keyboard(twitch::Keyboard keyboard) noexcept { loaded().keyboard = keyboard; }
void set_navigation_sounds(bool on) noexcept { loaded().navigationSounds = on; }
void set_lightbar(bool on) noexcept { loaded().lightbar = on; }
void set_debug_overlay(bool on) noexcept { loaded().debugOverlay = on; }
void set_chat_text_size(twitch::ChatTextSize size) noexcept { loaded().chatTextSize = size; }
void set_third_party_emotes(bool on) noexcept { loaded().thirdPartyEmotes = on; }
void set_live_notifications(bool on) noexcept { loaded().liveNotifications = on; }

bool save_settings(const twitch::AppSettings& s) noexcept {
  try {
    stored() = s;
    if (!evo_jailbreak_is_open()) return false;

    // evo_data_path() reuses a static buffer, so the .tmp path is copied out
    // before the second call asks for the real one.
    char tmpPath[256];
    snprintf(tmpPath, sizeof tmpPath, "%s", evo_data_path(kSettingsTmpFile));

    const std::string json = twitch::serializeSettings(s);

    FILE* f = fopen(tmpPath, "wb");
    if (!f) {
      evo_log_warn("twitch: could not open %s to save settings", tmpPath);
      return false;
    }
    const size_t written = fwrite(json.data(), 1, json.size(), f);
    fflush(f);
    fclose(f);
    if (written != json.size()) {
      evo_log_warn("twitch: short write saving settings to %s", tmpPath);
      return false;
    }

    const char* finalPath = evo_data_path(kSettingsFile);
    if (rename(tmpPath, finalPath) != 0) {
      // Same recovery as evo_usb_remote.c: drop the old file and retry once
      // before giving up.
      remove(finalPath);
      if (rename(tmpPath, finalPath) != 0) {
        evo_log_warn("twitch: could not move %s to %s", tmpPath, finalPath);
        return false;
      }
    }
    return true;
  } catch (...) {
    evo_log_warn("twitch: save_settings failed with an exception");
    return false;
  }
}

}  // namespace evo_twitch
