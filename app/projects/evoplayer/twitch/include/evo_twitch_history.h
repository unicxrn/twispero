#pragma once
// evo_twitch_history.h - what the user watched and searched for, in
// <data root>/twitch_history.json.
//
// The channels watched live (Home's "Recently watched"; the launcher records
// one each time a live stream starts, logged in or not), at most 30, the
// most recent first, and the last 8 searches (Search shows them before a
// query is typed), the most recent first (twitch::WatchHistory in the core).
//
// The file is read the first time anything here is asked after the sandbox
// opens (as the settings are). Every change is saved on its own: the list is
// copied on the main thread and serialized and written on a short-lived
// worker, to a .tmp file that is flushed to the disk and then renamed over
// the old one, so a crash or a power cut leaves the old file or the new one,
// never half of one. A change while the last save is still being written
// waits for it (history_frame()). The same pattern as evo_twitch_progress.h.
//
// Main thread only, except where it says otherwise. Nothing here throws.
// No core headers here: the screens include this through
// TwitchScreenData.hpp.

#include <string>
#include <vector>

namespace evo_twitch {

// One channel watched live (twitch::WatchedChannel).
struct HistoryChannel {
  std::string login;
  std::string name;       // display name, "" when not known (show the login)
  std::string avatarUrl;  // "" when not known
  long long watched = 0;  // unix seconds
};

// The channels, newest first, at most 30. The same object for the life of the
// app; it changes only inside the calls below and the first read of the file.
const std::vector<HistoryChannel>& history_channels() noexcept;

// The searches, newest first, at most 8. The same object for the life of the app.
const std::vector<std::string>& history_searches() noexcept;

// Goes up whenever either list changes.
unsigned history_revision() noexcept;

// A live stream of `login` started: it goes first (stamped with the time
// now), with its name and picture (empty ones keep what was kept). Saved.
void history_record_channel(const std::string& login, const std::string& name,
                            const std::string& avatarUrl) noexcept;

// A search was made (trimmed; blank ones are ignored): it goes first. Saved.
void history_record_search(const std::string& query) noexcept;

// Takes a search out (Square on Search's list). Saved when there was one.
void history_remove_search(const std::string& query) noexcept;

// Forgets every channel watched; the searches stay. Saved.
void history_clear_channels() noexcept;

// Writes the file now, on the calling thread, if anything changed: for the
// app's last moments, when a worker might not get to finish. A write under
// way finishes first (up to 3 s). True when the file was written.
bool history_save_now() noexcept;

// Once per frame: starts a save that waited for the writer.
void history_frame() noexcept;

// True while a write is under way or waiting. For tests.
bool history_saving() noexcept;

}  // namespace evo_twitch
