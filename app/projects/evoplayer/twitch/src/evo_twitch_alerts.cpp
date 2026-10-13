// evo_twitch_alerts.cpp - see evo_twitch_alerts.h.
//
// State lives in file-scope static storage, placement-new'd on first use and
// never destroyed (as the other services). Everything here runs on the main
// thread: the polling is only a request to the data service, whose follows
// worker does the asking.

#include "evo_twitch_alerts.h"

#include <deque>
#include <iterator>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "evo_boot_log.h"
#include "evo_twitch.h"
#include "evo_twitch_auth.h"
#include "evo_twitch_data.h"
#include "evo_twitch_settings.h"
#include "evo_twitch_text.h"
#include "twitch/notify.hpp"

namespace evo_twitch {

namespace {

constexpr unsigned long long kPollMs = 2 * 60 * 1000;      // ask again this long after the last answer
constexpr int kMaxPerAnswer = 3;                           // notes from one answer
constexpr unsigned long long kRepeatMs = 30 * 60 * 1000;   // a channel noted once waits this long
constexpr unsigned long long kStaleNoteMs = 60 * 1000;     // a note that waited longer is dropped
constexpr size_t kMaxWaiting = 3;

struct Note {
  std::string text;      // "<name> is live: <title>"
  std::string headline;  // "<name> is live"
  std::string detail;    // the title, cut as in text ("" when it has none)
  unsigned long long queuedMs = 0;
};

struct State {
  bool watching = false;     // logged in with the setting on
  std::string userId;        // whose list, while watching
  bool haveBaseline = false; // an answer has been seen since watching began
  std::vector<std::string> baseline;  // its logins
  unsigned long long seenStamp = 0;   // the answer last looked at
  unsigned long long askedMs = 0;     // when we last asked
  std::unordered_map<std::string, unsigned long long> noted;  // login -> when it was noted
  std::deque<Note> waiting;
};

alignas(State) unsigned char g_storage[sizeof(State)];
State* g_state = nullptr;

State& state() noexcept {
  if (!g_state) g_state = new (g_storage) State();
  return *g_state;
}

// The channel playing live now, "" when none (a video or clip doesn't count:
// its channel going live is news).
std::string playingLive() {
  const evo_twitch_now_playing_t* np = evo_twitch_now_playing();
  if (!np || np->video_type != 0) return std::string();
  return np->login;
}

void stopWatching(State& s) {
  s.watching = false;
  s.userId.clear();
  s.haveBaseline = false;
  s.baseline.clear();
  s.seenStamp = 0;
  s.waiting.clear();
}

// A new answer: remembered, and compared with the last one.
void look(State& s, const std::vector<StreamCard>& live, unsigned long long now) {
  std::vector<std::string> logins;
  logins.reserve(live.size());
  for (const StreamCard& c : live) logins.push_back(c.login);
  if (!s.haveBaseline) {
    s.haveBaseline = true;
    s.baseline = std::move(logins);
    evo_log("twitch: live notifications: %d followed channels live to start with",
            static_cast<int>(s.baseline.size()));
    return;
  }
  const std::vector<std::string> fresh = twitch::newlyLive(s.baseline, logins, playingLive(), kMaxPerAnswer);
  s.baseline = std::move(logins);
  for (auto it = s.noted.begin(); it != s.noted.end();)
    it = now - it->second > kRepeatMs ? s.noted.erase(it) : std::next(it);
  for (const std::string& login : fresh) {
    if (s.noted.count(login)) continue;
    s.noted[login] = now;
    for (const StreamCard& c : live) {
      if (c.login != login) continue;
      // The toast's font: what it can't draw is left out (evo_twitch_text.h).
      const std::string name = evo_twitch_text::channelName(c.displayName, c.login, true);
      Note note;
      note.text = twitch::goLiveText(name, evo_twitch_text::drawable(c.title, true));
      // The toast's two lines: what goLiveText puts after "<name> is live: ".
      note.headline = twitch::goLiveText(name, std::string());
      const std::string lead = note.headline + ": ";
      if (note.text.compare(0, lead.size(), lead) == 0) note.detail = note.text.substr(lead.size());
      note.queuedMs = now;
      s.waiting.push_back(std::move(note));
      while (s.waiting.size() > kMaxWaiting) s.waiting.pop_front();
      evo_log("twitch: live notifications: %s went live", c.login.c_str());
      break;
    }
  }
}

}  // namespace

void live_alerts_frame() noexcept {
  try {
    State& s = state();
    const AuthView& v = auth_view();
    const bool on = v.state == AuthState::LoggedIn && !v.userId.empty() && settings().liveNotifications;
    const unsigned long long now = data_now_ms();
    if (!on) {
      if (s.watching) stopWatching(s);
      return;
    }
    if (!s.watching || s.userId != v.userId) {
      stopWatching(s);
      s.watching = true;
      s.userId = v.userId;
      s.askedMs = now;  // the screens ask on their own as the login lands
    }

    const Result<std::vector<StreamCard>>& r = followed_live();
    if (r.status == Status::Ready && r.stamp != 0 && r.stamp != s.seenStamp) {
      s.seenStamp = r.stamp;
      look(s, r.value, now);
    }

    // Every 2 minutes from the last answer or the last ask, whichever is later.
    const unsigned long long last = r.stamp > s.askedMs ? r.stamp : s.askedMs;
    if (now - last >= kPollMs) {
      s.askedMs = now;
      request_followed_live(s.userId);
    }
  } catch (...) {
    evo_log_error("twitch: live notifications failed this frame");
  }
}

namespace {

// The next note, the stale ones dropped first; nullptr when none waits.
Note* next(State& s) {
  const unsigned long long now = data_now_ms();
  while (!s.waiting.empty() && now - s.waiting.front().queuedMs > kStaleNoteMs) s.waiting.pop_front();
  return s.waiting.empty() ? nullptr : &s.waiting.front();
}

}  // namespace

bool live_alerts_take(std::string* out) noexcept {
  try {
    State& s = state();
    Note* note = next(s);
    if (!note || !out) return false;
    *out = std::move(note->text);
    s.waiting.pop_front();
    return true;
  } catch (...) {
    return false;
  }
}

bool live_alerts_take(std::string* headline, std::string* detail) noexcept {
  try {
    State& s = state();
    Note* note = next(s);
    if (!note || !headline || !detail) return false;
    *headline = std::move(note->headline);
    *detail = std::move(note->detail);
    s.waiting.pop_front();
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace evo_twitch
