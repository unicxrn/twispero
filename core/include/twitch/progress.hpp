#pragma once
#include <string>
#include <vector>

namespace twitch {

// How far a video (a past broadcast, highlight or upload) was watched, and
// what a "Continue watching" card shows of it. Clips are short and don't get
// one.
struct WatchProgress {
  std::string id;         // the video's id
  int seconds = 0;        // where it was left
  int duration = 0;       // its length; 0 when not known
  long long updated = 0;  // unix seconds of the last save
  std::string title;
  std::string channel;    // display name
  std::string login;      // the channel's login, "" when not known
  std::string thumb;      // the 440x248 thumbnail
  std::string game;       // category, "" when none
  std::string published;  // ISO 8601 as Twitch sends it, "" when not known (see ageText)
  std::string type;       // "archive", "highlight" or "upload", "" when not known
};

inline constexpr int kProgressCap = 200;
// Resuming only makes sense between these: before the first 30 s it starts
// from the top, and in the last minute it counts as watched.
inline constexpr int kResumeMinSeconds = 30;
inline constexpr int kResumeEndMargin = 60;

// Where to pick e up again: its seconds when they are past kResumeMinSeconds
// and more than kResumeEndMargin before the end (the length must be known),
// otherwise 0, from the top.
int resumeAt(const WatchProgress& e);

// How much of e was watched, 0..1; -1 when its length isn't known.
float watchedFraction(const WatchProgress& e);

// Every video's progress, the most recently updated first, at most
// kProgressCap of them: recording one more drops the oldest.
class ProgressStore {
 public:
  // Adds e, or replaces the entry with its id, and puts it first. An entry
  // with an empty id is ignored.
  void record(const WatchProgress& e);
  // nullptr when there is none for id.
  const WatchProgress* find(const std::string& id) const;
  // True when there was one to remove.
  bool remove(const std::string& id);
  void clear() { entries_.clear(); }
  const std::vector<WatchProgress>& entries() const { return entries_; }
  // The videos to continue (resumeAt() > 0), most recent first, at most max.
  std::vector<WatchProgress> continueWatching(int max) const;

 private:
  std::vector<WatchProgress> entries_;
};

// The file: {"videos": {"<id>": {"seconds", "duration", "updated", "title",
// "channel", "login", "thumb", "game", "published", "type"}}}. Never throws:
// a broken file reads as empty, and an entry with a missing id or seconds is
// left out. Kept to the kProgressCap most recent, newest first.
ProgressStore parseProgress(const std::string& json) noexcept;
std::string serializeProgress(const ProgressStore& store);

// "1:23:45", "12:34", "0:05" (a negative time reads 0:00).
std::string clockText(int seconds);
// What is left of a video: "42 min left", "1 h left", "1 h 12 min left"; a
// part of a minute counts as a minute.
std::string timeLeftText(int seconds, int duration);

}  // namespace twitch
