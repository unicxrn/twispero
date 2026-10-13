#include "twitch/hls.hpp"

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <map>

#include "twitch/error.hpp"

namespace twitch {
namespace {

std::vector<std::string> splitLines(const std::string& text) {
  std::vector<std::string> lines;
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find('\n', start);
    std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (end == std::string::npos) {
      if (!line.empty()) lines.push_back(line);
      break;
    }
    lines.push_back(line);
    start = end + 1;
  }
  return lines;
}

bool startsWith(const std::string& s, const char* prefix) {
  return s.rfind(prefix, 0) == 0;
}

// KEY=VALUE,KEY="quoted, value",... -> map. Quotes are removed.
std::map<std::string, std::string> parseAttributes(const std::string& list) {
  std::map<std::string, std::string> attrs;
  size_t i = 0;
  while (i < list.size()) {
    size_t eq = list.find('=', i);
    if (eq == std::string::npos) break;
    std::string key = list.substr(i, eq - i);
    std::string value;
    size_t j = eq + 1;
    if (j < list.size() && list[j] == '"') {
      size_t close = list.find('"', j + 1);
      if (close == std::string::npos) close = list.size();
      value = list.substr(j + 1, close - j - 1);
      j = close + 1;
    } else {
      size_t comma = list.find(',', j);
      if (comma == std::string::npos) comma = list.size();
      value = list.substr(j, comma - j);
      j = comma;
    }
    attrs[key] = value;
    i = (j < list.size() && list[j] == ',') ? j + 1 : j;
  }
  return attrs;
}

// Parses a non-negative decimal int; returns 0 for empty, non-numeric or out-of-range text.
int parseIntOrZero(const std::string& s) {
  char* end = nullptr;
  errno = 0;
  long v = std::strtol(s.c_str(), &end, 10);
  if (end == s.c_str() || errno == ERANGE || v < 0 || v > INT_MAX) return 0;
  return static_cast<int>(v);
}

// Parses a non-negative decimal long long; returns 0 for empty, non-numeric or out-of-range text.
long long parseLongLongOrZero(const std::string& s) {
  char* end = nullptr;
  errno = 0;
  long long v = std::strtoll(s.c_str(), &end, 10);
  if (end == s.c_str() || errno == ERANGE || v < 0) return 0;
  return v;
}

void requireM3u(const std::vector<std::string>& lines) {
  if (lines.empty() || !startsWith(lines[0], "#EXTM3U")) throw ApiError("not an M3U playlist");
}

}  // namespace

bool MediaPlaylist::containsAds() const {
  if (hasAdDateRange) return true;
  for (const auto& s : segments)
    if (s.ad) return true;
  return false;
}

std::vector<Variant> parseMaster(const std::string& text) {
  auto lines = splitLines(text);
  requireM3u(lines);

  // GROUP-ID -> human NAME, from every video EXT-X-MEDIA line. A duplicate GROUP-ID
  // (seen twice in the playlist) keeps the last NAME encountered.
  std::map<std::string, std::string> labels;
  for (const auto& line : lines) {
    if (!startsWith(line, "#EXT-X-MEDIA:")) continue;
    auto attrs = parseAttributes(line.substr(sizeof("#EXT-X-MEDIA:") - 1));
    if (attrs["TYPE"] != "VIDEO") continue;
    const std::string& groupId = attrs["GROUP-ID"];
    if (groupId.empty()) continue;
    labels[groupId] = attrs["NAME"];
  }

  std::vector<Variant> out;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (!startsWith(lines[i], "#EXT-X-STREAM-INF:")) continue;
    auto attrs = parseAttributes(lines[i].substr(sizeof("#EXT-X-STREAM-INF:") - 1));

    Variant v;
    v.name = attrs["VIDEO"];
    v.bandwidth = parseIntOrZero(attrs["BANDWIDTH"]);
    v.fps = std::atof(attrs["FRAME-RATE"].c_str());
    const std::string& res = attrs["RESOLUTION"];
    size_t x = res.find('x');
    if (x != std::string::npos) {
      v.width = parseIntOrZero(res.substr(0, x));
      v.height = parseIntOrZero(res.substr(x + 1));
    }

    bool foundUri = false;
    for (size_t j = i + 1; j < lines.size(); ++j) {
      // Stop if another STREAM-INF is encountered without finding a URI
      if (startsWith(lines[j], "#EXT-X-STREAM-INF:")) break;
      // Found a non-comment line: it's the URI
      if (!lines[j].empty() && lines[j][0] != '#') {
        v.uri = lines[j];
        i = j;
        foundUri = true;
        break;
      }
    }

    if (!foundUri) continue;

    if (v.name.empty()) v.name = std::to_string(v.height) + "p";

    auto it = labels.find(v.name);
    v.label = (it != labels.end() && !it->second.empty()) ? it->second : v.name;
    v.source = (v.name == "chunked") || v.label.find("(source)") != std::string::npos;

    out.push_back(std::move(v));
  }
  return out;
}

MediaPlaylist parseMedia(const std::string& text) {
  auto lines = splitLines(text);
  requireM3u(lines);

  MediaPlaylist pl;
  std::string currentMap;
  bool pendingDiscontinuity = false;
  bool havePendingInf = false;
  double pendingDuration = 0;
  std::string pendingTitle;

  for (const auto& line : lines) {
    if (line.empty()) continue;

    if (startsWith(line, "#EXT-X-TARGETDURATION:")) {
      pl.targetDuration = std::atof(line.c_str() + sizeof("#EXT-X-TARGETDURATION:") - 1);
    } else if (startsWith(line, "#EXT-X-MEDIA-SEQUENCE:")) {
      pl.mediaSequence = parseLongLongOrZero(line.substr(sizeof("#EXT-X-MEDIA-SEQUENCE:") - 1));
    } else if (startsWith(line, "#EXT-X-MAP:")) {
      currentMap = parseAttributes(line.substr(sizeof("#EXT-X-MAP:") - 1))["URI"];
    } else if (line == "#EXT-X-DISCONTINUITY") {
      pendingDiscontinuity = true;
    } else if (startsWith(line, "#EXT-X-DATERANGE:")) {
      if (line.find("CLASS=\"twitch-stitched-ad\"") != std::string::npos) pl.hasAdDateRange = true;
    } else if (startsWith(line, "#EXT-X-ENDLIST")) {
      pl.endList = true;
    } else if (startsWith(line, "#EXTINF:")) {
      std::string rest = line.substr(sizeof("#EXTINF:") - 1);
      size_t comma = rest.find(',');
      pendingDuration = std::atof(rest.substr(0, comma).c_str());
      pendingTitle = comma == std::string::npos ? "" : rest.substr(comma + 1);
      havePendingInf = true;
    } else if (line[0] != '#' && havePendingInf) {
      Segment s;
      s.sequence = pl.mediaSequence + static_cast<long long>(pl.segments.size());
      s.duration = pendingDuration;
      s.title = pendingTitle;
      s.uri = line;
      s.mapUri = currentMap;
      s.discontinuity = pendingDiscontinuity;
      s.ad = !s.title.empty() && s.title != "live";
      pl.segments.push_back(std::move(s));
      pendingDiscontinuity = false;
      havePendingInf = false;
    }
  }
  return pl;
}

}  // namespace twitch
