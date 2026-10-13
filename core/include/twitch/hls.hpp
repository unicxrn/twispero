#pragma once
#include <string>
#include <vector>

namespace twitch {

// One quality from the master playlist.
struct Variant {
  std::string name;  // stable key from VIDEO="..." (GROUP-ID), e.g. "chunked", "720p60", "720p30"
  int bandwidth = 0;
  int width = 0;
  int height = 0;
  double fps = 0;
  std::string uri;      // media playlist URL
  std::string label;    // human name from EXT-X-MEDIA NAME, e.g. "1080p60 (source)", "720p"; falls back to name
  bool source = false;  // the original-quality rendition (GROUP-ID "chunked" or NAME containing "(source)")
};

struct Segment {
  long long sequence = 0;
  double duration = 0;
  std::string title;   // "live", "Amazon|123..." (ad), or "" (VOD)
  std::string uri;
  std::string mapUri;  // EXT-X-MAP init segment in effect for this segment ("" if none)
  bool discontinuity = false;
  bool ad = false;     // Twitch-specific: title is set and is not "live"
};

struct MediaPlaylist {
  double targetDuration = 0;
  long long mediaSequence = 0;
  bool hasAdDateRange = false;  // contains CLASS="twitch-stitched-ad"
  bool endList = false;         // VOD / finished stream
  std::vector<Segment> segments;

  bool containsAds() const;
};

// Throws ApiError when the text is not an M3U playlist.
std::vector<Variant> parseMaster(const std::string& text);
MediaPlaylist parseMedia(const std::string& text);

}  // namespace twitch
