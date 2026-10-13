#include "twitch/notify.hpp"

#include <algorithm>
#include <cctype>

namespace twitch {

namespace {

std::string lowerAscii(const std::string& s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

bool contains(const std::vector<std::string>& lowered, const std::string& key) {
  return std::find(lowered.begin(), lowered.end(), key) != lowered.end();
}

}  // namespace

std::vector<std::string> newlyLive(const std::vector<std::string>& before, const std::vector<std::string>& now,
                                   const std::string& playing, int max) {
  std::vector<std::string> out;
  if (max <= 0) return out;
  std::vector<std::string> seen;  // lower case: the ones before, and the ones taken
  seen.reserve(before.size() + now.size() + 1);
  for (const std::string& b : before) seen.push_back(lowerAscii(b));
  if (!playing.empty()) seen.push_back(lowerAscii(playing));
  for (const std::string& n : now) {
    if (static_cast<int>(out.size()) >= max) break;
    const std::string key = lowerAscii(n);
    if (key.empty() || contains(seen, key)) continue;
    seen.push_back(key);
    out.push_back(n);
  }
  return out;
}

std::string goLiveText(const std::string& name, const std::string& title) {
  if (title.empty()) return name + " is live";
  // Count characters (UTF-8 lead bytes), cutting before the one past the limit.
  size_t cut = title.size();
  int chars = 0;
  for (size_t i = 0; i < title.size(); ++i) {
    if ((static_cast<unsigned char>(title[i]) & 0xC0) == 0x80) continue;
    if (chars == kGoLiveTitleChars) {
      cut = i;
      break;
    }
    ++chars;
  }
  std::string shown = title.substr(0, cut);
  if (cut < title.size()) {
    while (!shown.empty() && shown.back() == ' ') shown.pop_back();
    shown += "\xE2\x80\xA6";  // ...
  }
  return name + " is live: " + shown;
}

}  // namespace twitch
