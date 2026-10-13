#pragma once
#include <string>
#include <vector>

namespace twitch {

// A channel the user watched live, for Home's "Recently watched".
struct WatchedChannel {
  std::string login;        // lower case, Twitch's rules (normalizeLogin)
  std::string displayName;  // "" when not known (the screens show the login)
  std::string avatarUrl;    // "" when not known
  long long watched = 0;    // unix seconds when it was last watched
};

inline constexpr int kHistoryCap = 30;       // channels kept, the most recent
inline constexpr int kRecentSearchCap = 8;   // searches kept, the most recent
inline constexpr size_t kMaxSearchBytes = 100;  // a longer one is cut (at a character)

// What the user watched and searched for: the channels watched live, newest
// first, and the last searches, newest first.
class WatchHistory {
 public:
  // Puts c first: a channel already there moves up, taking c's name and
  // picture unless c has none (then it keeps its own). The oldest one goes
  // past kHistoryCap. A login that isn't a valid one is ignored.
  void recordChannel(const WatchedChannel& c);
  // True when there was one to remove.
  bool removeChannel(const std::string& login);
  void clearChannels() { channels_.clear(); }
  const std::vector<WatchedChannel>& channels() const { return channels_; }

  // Puts a search first: trimmed, cut to kMaxSearchBytes, the same words
  // again (any case) only move up. A blank one is ignored. The oldest one
  // goes past kRecentSearchCap.
  void recordSearch(const std::string& query);
  // Removes a search by its words (any case). True when there was one.
  bool removeSearch(const std::string& query);
  void clearSearches() { searches_.clear(); }
  const std::vector<std::string>& searches() const { return searches_; }

 private:
  std::vector<WatchedChannel> channels_;
  std::vector<std::string> searches_;
};

// The file: {"channels": [{"login", "name", "avatar", "watched"}, ...],
// "searches": ["...", ...]}, both newest first. Never throws: a broken file
// reads as empty, a part of it that is broken as empty, and an entry that
// isn't valid is left out. Kept to the caps.
WatchHistory parseHistory(const std::string& json) noexcept;
std::string serializeHistory(const WatchHistory& history);

}  // namespace twitch
