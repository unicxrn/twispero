#pragma once
// evo_twitch_progress.h - how far each past broadcast, highlight or upload
// was watched, in <data root>/twitch_progress.json.
//
// The launcher records where a video is while it plays (every 15 s, and once
// more when it stops) and asks where to resume one; the screens draw the
// cards' progress bars and Home's "Continue watching" from it. At most 200
// videos, the most recent first (twitch::ProgressStore in the core).
//
// The file is read the first time anything here is asked after the sandbox
// opens (as the settings are). A save copies the list on the main thread and
// serializes and writes it on a short-lived worker, to a .tmp file that is
// flushed to the disk and then renamed over the old one, so a crash or a
// power cut leaves the old file or the new one, never half of one. A save
// while the last one is still being written waits for it (progress_frame()).
//
// Main thread only, except where it says otherwise. Nothing here throws.
// No core headers here: every screen includes this through
// TwitchScreenData.hpp.

#include <string>
#include <vector>

namespace evo_twitch {

// One video's saved progress (twitch::WatchProgress).
struct VideoProgress {
  std::string id;
  int seconds = 0;        // where it was left
  int duration = 0;       // its length, 0 when not known
  long long updated = 0;  // unix seconds of the last save
  std::string title, channel, login, thumb, game;
  std::string published;  // ISO 8601, "" when not known
  std::string type;       // "archive", "highlight", "upload" or ""
};

// How much of video `id` was watched, 0..1, for its card's bar; -1 when
// nothing is saved for it (or its length isn't known).
float progress_fraction(const std::string& id) noexcept;

// Where to pick video `id` up again, in seconds; 0 to start from the top
// (nothing saved, the first 30 s, the last minute: twitch::resumeAt).
int progress_resume_at(const std::string& id) noexcept;

// The saved progress of video `id`, or nullptr. Good until the next record.
const VideoProgress* progress_find(const std::string& id) noexcept;

// "Continue watching": the videos that can be resumed, newest first, at most
// 20. The same object for the life of the app; it changes only inside
// progress_record() and the first read of the file.
const std::vector<VideoProgress>& progress_continue() noexcept;

// Goes up whenever anything above changes.
unsigned progress_revision() noexcept;

// "3 days ago" for v's date, as of now; "" when it has none.
std::string progress_age(const VideoProgress& v) noexcept;

// Keeps where e.id is (stamped with the time now, and put first). Not
// written until progress_save().
void progress_record(const VideoProgress& e) noexcept;

// Forgets every video's progress (Settings > System > Clear watch history):
// Continue watching empties and the cards lose their bars. Not written until
// progress_save().
void progress_clear() noexcept;

// Writes the file on a worker if anything changed since the last write. A
// save while the last one is still being written goes once that one is done.
// False when there is nothing to do or the sandbox isn't open yet.
bool progress_save() noexcept;

// Writes the file now, on the calling thread, if anything changed: for the
// app's last moments, when a worker might not get to finish. A write under
// way finishes first (up to 3 s). True when the file was written.
bool progress_save_now() noexcept;

// Once per frame: starts a save that waited for the writer.
void progress_frame() noexcept;

// True while a write is under way or waiting. For tests.
bool progress_saving() noexcept;

}  // namespace evo_twitch
