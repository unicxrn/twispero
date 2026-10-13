#pragma once
// evo_twitch_settings.h - the app's settings, in <data root>/twitch_settings.json.
//
// This is the only settings file: the Twitch choices (default quality, chat,
// its text size, third-party emotes, live notifications) and the app's own
// (keyboard, navigation sounds, controller light, debug overlay). EVO's
// SettingsService reads and writes it through here.
//
// The data root is /data/ps5twitch once the app has lifted its sandbox
// (evo_jailbreak_is_open()), which can take a few seconds after boot. Until
// then settings() returns the defaults and tries the file again on the next
// call; the first call after the sandbox opens reads it. A missing or broken
// file means defaults. Main thread only. Nothing here throws.

#include "twitch/settings.hpp"

namespace evo_twitch {

const twitch::AppSettings& settings() noexcept;

// Keeps `s` for this run and writes it (to a .tmp file, then rename). False
// when the file could not be written (always, before the sandbox opens); the
// change still holds until the app closes.
bool save_settings(const twitch::AppSettings& s) noexcept;

// Change one setting for this run without writing the file;
// save_settings(settings()) writes them. For changes that come in bursts (R1
// in the player) and for Settings, which writes when it is left.
void set_quality(const char* name) noexcept;  // not one of twitch::kQualities: "auto"
void set_chat_mode(twitch::ChatMode mode) noexcept;
void set_chat_hint_seen(bool seen) noexcept;
void set_keyboard(twitch::Keyboard keyboard) noexcept;
void set_navigation_sounds(bool on) noexcept;
void set_lightbar(bool on) noexcept;
void set_debug_overlay(bool on) noexcept;
void set_chat_text_size(twitch::ChatTextSize size) noexcept;
void set_third_party_emotes(bool on) noexcept;
void set_live_notifications(bool on) noexcept;

}  // namespace evo_twitch
