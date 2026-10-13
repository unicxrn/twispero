#include "twitch/history.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

#include <nlohmann/json.hpp>

#include "twitch/error.hpp"
#include "twitch/login.hpp"

namespace twitch {

namespace {

using Json = nlohmann::json;

// The login in Twitch's own form, or "" when it can't be one.
std::string loginOf(const std::string& login) {
  try {
    return normalizeLogin(login);
  } catch (const NotFoundError&) {
    return std::string();
  }
}

std::string lowerAscii(const std::string& s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

// s without the white space at its ends, cut to max bytes without cutting a
// UTF-8 character in two.
std::string tidy(const std::string& s, size_t max) {
  const size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return std::string();
  const size_t e = s.find_last_not_of(" \t\r\n");
  std::string out = s.substr(b, e - b + 1);
  if (out.size() > max) {
    size_t n = max;
    while (n > 0 && (static_cast<unsigned char>(out[n]) & 0xC0) == 0x80) --n;
    out.resize(n);
    const size_t last = out.find_last_not_of(" \t\r\n");
    out.resize(last == std::string::npos ? 0 : last + 1);
  }
  return out;
}

std::string stringField(const Json& j, const char* key) {
  const auto it = j.find(key);
  return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

}  // namespace

void WatchHistory::recordChannel(const WatchedChannel& c) {
  WatchedChannel entry = c;
  entry.login = loginOf(c.login);
  if (entry.login.empty()) return;
  const auto it = std::find_if(channels_.begin(), channels_.end(),
                               [&](const WatchedChannel& w) { return w.login == entry.login; });
  if (it != channels_.end()) {
    if (entry.displayName.empty()) entry.displayName = it->displayName;
    if (entry.avatarUrl.empty()) entry.avatarUrl = it->avatarUrl;
    channels_.erase(it);
  }
  channels_.insert(channels_.begin(), std::move(entry));
  if (channels_.size() > static_cast<size_t>(kHistoryCap)) channels_.resize(static_cast<size_t>(kHistoryCap));
}

bool WatchHistory::removeChannel(const std::string& login) {
  const std::string key = loginOf(login);
  const auto it = std::find_if(channels_.begin(), channels_.end(),
                               [&](const WatchedChannel& w) { return w.login == key; });
  if (key.empty() || it == channels_.end()) return false;
  channels_.erase(it);
  return true;
}

void WatchHistory::recordSearch(const std::string& query) {
  const std::string q = tidy(query, kMaxSearchBytes);
  if (q.empty()) return;
  removeSearch(q);
  searches_.insert(searches_.begin(), q);
  if (searches_.size() > static_cast<size_t>(kRecentSearchCap))
    searches_.resize(static_cast<size_t>(kRecentSearchCap));
}

bool WatchHistory::removeSearch(const std::string& query) {
  const std::string key = lowerAscii(tidy(query, kMaxSearchBytes));
  const auto it = std::find_if(searches_.begin(), searches_.end(),
                               [&](const std::string& s) { return lowerAscii(s) == key; });
  if (key.empty() || it == searches_.end()) return false;
  searches_.erase(it);
  return true;
}

WatchHistory parseHistory(const std::string& json) noexcept {
  WatchHistory history;
  try {
    const Json j = Json::parse(json, nullptr, false);
    if (!j.is_object()) return history;

    if (const auto channels = j.find("channels"); channels != j.end() && channels->is_array()) {
      std::vector<WatchedChannel> all;
      for (const Json& c : *channels) {
        if (!c.is_object()) continue;
        WatchedChannel w;
        w.login = stringField(c, "login");
        w.displayName = stringField(c, "name");
        w.avatarUrl = stringField(c, "avatar");
        if (const auto t = c.find("watched"); t != c.end() && t->is_number_integer()) w.watched = t->get<long long>();
        all.push_back(std::move(w));
      }
      // Newest first; the file's own order (newest first too) breaks a tie.
      std::stable_sort(all.begin(), all.end(),
                       [](const WatchedChannel& a, const WatchedChannel& b) { return a.watched > b.watched; });
      // recordChannel() puts each one first and drops repeats and bad logins,
      // so the oldest goes in first; a login seen twice keeps its newest entry.
      for (auto it = all.rbegin(); it != all.rend(); ++it) history.recordChannel(*it);
    }

    if (const auto searches = j.find("searches"); searches != j.end() && searches->is_array()) {
      for (auto it = searches->rbegin(); it != searches->rend(); ++it)
        if (it->is_string()) history.recordSearch(it->get<std::string>());
    }
  } catch (...) {
    return WatchHistory{};
  }
  return history;
}

std::string serializeHistory(const WatchHistory& history) {
  Json channels = Json::array();
  for (const WatchedChannel& c : history.channels()) {
    Json o;
    o["login"] = c.login;
    o["name"] = c.displayName;
    o["avatar"] = c.avatarUrl;
    o["watched"] = c.watched;
    channels.push_back(std::move(o));
  }
  Json searches = Json::array();
  for (const std::string& s : history.searches()) searches.push_back(s);
  Json j;
  j["channels"] = std::move(channels);
  j["searches"] = std::move(searches);
  // A name or a search can hold any bytes; replace what isn't UTF-8 rather
  // than throw.
  return j.dump(-1, ' ', false, Json::error_handler_t::replace) + "\n";
}

}  // namespace twitch
