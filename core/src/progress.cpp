#include "twitch/progress.hpp"

#include <algorithm>
#include <cstdio>
#include <utility>

#include <nlohmann/json.hpp>

namespace twitch {

namespace {

// A video id Twitch would hand out is a short string of digits; anything that
// is empty or absurdly long can't be one.
constexpr size_t kMaxIdBytes = 64;

// Objects keep their keys in the order written: the file lists the videos
// newest first, which breaks a tie between two saved in the same second.
using Json = nlohmann::ordered_json;

std::string stringField(const Json& j, const char* key) {
  const auto it = j.find(key);
  return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// A whole number of seconds from a JSON number; false when the field is
// missing, not a number, or out of range.
bool secondsField(const Json& j, const char* key, int& out) {
  const auto it = j.find(key);
  if (it == j.end() || !it->is_number()) return false;
  const double d = it->get<double>();
  if (!(d >= 0.0) || d > 1.0e9) return false;
  out = static_cast<int>(d);
  return true;
}

}  // namespace

int resumeAt(const WatchProgress& e) {
  if (e.duration <= 0) return 0;
  if (e.seconds <= kResumeMinSeconds || e.seconds >= e.duration - kResumeEndMargin) return 0;
  return e.seconds;
}

float watchedFraction(const WatchProgress& e) {
  if (e.duration <= 0) return -1.0f;
  const float f = static_cast<float>(e.seconds) / static_cast<float>(e.duration);
  return std::clamp(f, 0.0f, 1.0f);
}

void ProgressStore::record(const WatchProgress& e) {
  if (e.id.empty()) return;
  const auto it = std::find_if(entries_.begin(), entries_.end(),
                               [&](const WatchProgress& p) { return p.id == e.id; });
  if (it != entries_.end()) entries_.erase(it);
  entries_.insert(entries_.begin(), e);
  if (entries_.size() > static_cast<size_t>(kProgressCap)) entries_.resize(static_cast<size_t>(kProgressCap));
}

const WatchProgress* ProgressStore::find(const std::string& id) const {
  for (const WatchProgress& p : entries_)
    if (p.id == id) return &p;
  return nullptr;
}

bool ProgressStore::remove(const std::string& id) {
  const auto it = std::find_if(entries_.begin(), entries_.end(),
                               [&](const WatchProgress& p) { return p.id == id; });
  if (it == entries_.end()) return false;
  entries_.erase(it);
  return true;
}

std::vector<WatchProgress> ProgressStore::continueWatching(int max) const {
  std::vector<WatchProgress> out;
  for (const WatchProgress& p : entries_) {
    if (static_cast<int>(out.size()) >= max) break;
    if (resumeAt(p) > 0) out.push_back(p);
  }
  return out;
}

ProgressStore parseProgress(const std::string& json) noexcept {
  ProgressStore store;
  try {
    const Json j = Json::parse(json, nullptr, false);
    if (!j.is_object()) return store;
    const auto videos = j.find("videos");
    if (videos == j.end() || !videos->is_object()) return store;

    std::vector<WatchProgress> all;
    for (auto it = videos->begin(); it != videos->end(); ++it) {
      const Json& v = it.value();
      if (it.key().empty() || it.key().size() > kMaxIdBytes || !v.is_object()) continue;
      WatchProgress e;
      e.id = it.key();
      if (!secondsField(v, "seconds", e.seconds)) continue;
      secondsField(v, "duration", e.duration);
      if (const auto u = v.find("updated"); u != v.end() && u->is_number_integer()) e.updated = u->get<long long>();
      e.title = stringField(v, "title");
      e.channel = stringField(v, "channel");
      e.login = stringField(v, "login");
      e.thumb = stringField(v, "thumb");
      e.game = stringField(v, "game");
      e.published = stringField(v, "published");
      e.type = stringField(v, "type");
      all.push_back(std::move(e));
    }
    // Newest first; the file's own order (newest first too) breaks a tie.
    std::stable_sort(all.begin(), all.end(),
                     [](const WatchProgress& a, const WatchProgress& b) { return a.updated > b.updated; });
    if (all.size() > static_cast<size_t>(kProgressCap)) all.resize(static_cast<size_t>(kProgressCap));
    // record() puts each one first, so the oldest goes in first.
    for (auto it = all.rbegin(); it != all.rend(); ++it) store.record(*it);
  } catch (...) {
    return ProgressStore{};
  }
  return store;
}

std::string serializeProgress(const ProgressStore& store) {
  Json videos = Json::object();
  for (const WatchProgress& e : store.entries()) {
    Json v;
    v["seconds"] = e.seconds;
    v["duration"] = e.duration;
    v["updated"] = e.updated;
    v["title"] = e.title;
    v["channel"] = e.channel;
    v["login"] = e.login;
    v["thumb"] = e.thumb;
    v["game"] = e.game;
    v["published"] = e.published;
    v["type"] = e.type;
    videos[e.id] = std::move(v);
  }
  Json j;
  j["videos"] = std::move(videos);
  // A title can hold any bytes Twitch sent; replace what isn't UTF-8 rather
  // than throw.
  return j.dump(-1, ' ', false, Json::error_handler_t::replace) + "\n";
}

std::string clockText(int seconds) {
  const int s = std::max(0, seconds);
  char b[32];
  if (s >= 3600)
    std::snprintf(b, sizeof b, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
  else
    std::snprintf(b, sizeof b, "%d:%02d", s / 60, s % 60);
  return b;
}

std::string timeLeftText(int seconds, int duration) {
  const int left = std::max(0, duration - std::max(0, seconds));
  const int minutes = std::max(1, (left + 59) / 60);
  char b[48];
  if (minutes < 60)
    std::snprintf(b, sizeof b, "%d min left", minutes);
  else if (minutes % 60 == 0)
    std::snprintf(b, sizeof b, "%d h left", minutes / 60);
  else
    std::snprintf(b, sizeof b, "%d h %d min left", minutes / 60, minutes % 60);
  return b;
}

}  // namespace twitch
