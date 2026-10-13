#pragma once
#include <string>
#include <vector>

#include "twitch/gql.hpp"
#include "twitch/http.hpp"
#include "twitch/playback.hpp"  // AccessToken, Variant, pickQuality (reused for clips too)

namespace twitch {

// Twitch's own three kinds of entry in a channel's video list.
enum class VideoType { Archive, Highlight, Upload };

// Twitch's own clip time windows.
enum class ClipPeriod { Day, Week, Month, All };

struct VideoCard {
  std::string id;
  std::string title;
  int lengthSeconds = 0;
  int viewCount = 0;
  std::string thumbnailUrl;  // 440x248, the size the grid cards draw
  std::string publishedAt;   // ISO 8601, kept as Twitch sends it (see ageText)
  std::string gameName;      // empty when Twitch doesn't say
};

struct ClipCard {
  std::string slug;
  std::string title;
  int viewCount = 0;
  int durationSeconds = 0;
  std::string thumbnailUrl;  // 1920x1080 - thumbnailURL with no width/height args (verified live)
  std::string createdAt;     // ISO 8601, kept as Twitch sends it (see ageText)
  std::string curatorName;
  std::string gameName;
  std::string broadcasterLogin;  // the channel it was clipped from
  std::string broadcasterName;   // its display name, "" when Twitch gives none
};

struct VideoPage {
  std::vector<VideoCard> cards;
  std::string nextCursor;  // "" when there is no next page
};

struct ClipPage {
  std::vector<ClipCard> cards;
  std::string nextCursor;  // "" when there is no next page
};

// One page of a channel's videos (past broadcasts, highlights or uploads), newest first.
// after is a cursor from a previous page's nextCursor, or "" for the first page. Normalizes
// the login and throws NotFoundError for an invalid one, with no request sent.
VideoPage fetchChannelVideos(GqlClient& gql, const std::string& login, VideoType type, int first,
                             const std::string& after = "");

// One page of a channel's clips for a time window, most viewed first. Same cursor rule as
// fetchChannelVideos.
ClipPage fetchChannelClips(GqlClient& gql, const std::string& login, ClipPeriod period, int first,
                           const std::string& after = "");

// A category's clips for a time window, most viewed first (no pagination, like categoryStreams).
// Throws NotFoundError when the category doesn't exist.
std::vector<ClipCard> fetchCategoryClips(GqlClient& gql, const std::string& categoryName,
                                         ClipPeriod period, int first);

// The access token for a VOD. Throws NotFoundError when the video doesn't exist, and
// SubscriberOnlyError when the token's own authorization.forbidden already says playback is
// refused.
AccessToken fetchVodToken(GqlClient& gql, const std::string& videoId);

inline constexpr const char* kUsherVodBase = "https://usher.ttvnw.net/vod/";

// Master playlist URL for a VOD, built from its access token. Pure string work, no network -
// mirrors usherStreamUrl for the live path.
std::string vodPlaylistUrl(const std::string& videoId, const AccessToken& token);

// The one-call version the CLI and the app use: fetchVodToken, then vodPlaylistUrl, then
// fetchVariants on the result (same call the live path makes), so parseMaster/qualityMenu/
// pickQuality all work unchanged on a VOD. Throws NotFoundError for a missing video,
// SubscriberOnlyError for a subscriber-only one (from the token, or from usher's own 403).
std::vector<Variant> resolveVod(GqlClient& gql, HttpClient& http, const std::string& videoId);

struct ClipVariant {
  std::string quality;  // Twitch's height string, e.g. "1080", "720", "480", "360"
  int fps = 0;          // rounded frame rate, e.g. 60 or 30
  std::string url;      // sourceURL with ?sig=&token= appended (both url-encoded)
};

// A clip's playable qualities and their signed, playable URLs. Throws NotFoundError when the
// clip doesn't exist, including one that has been deleted.
std::vector<ClipVariant> clipVariants(GqlClient& gql, const std::string& slug);

// The variant to play for a quality the user asked for (a kQualities value, or a name from
// qualityMenu()). Ranks exactly like pickQuality (clips have no source marker and no
// audio-only rendition, so "source" and "audio_only" both fall back to the best one):
//   - auto/best/"" takes the tallest, then fastest;
//   - an exact height+rate match ("720p60", "720p" meaning 30 fps) wins;
//   - otherwise the closest one at or below it, or else the lowest one above it;
//   - a name that means nothing takes the best.
// Throws ApiError if variants is empty.
ClipVariant pickClipQuality(const std::vector<ClipVariant>& variants, const std::string& wanted);

// "3 days ago", "5 hours ago", "just now", and so on, for an ISO 8601 UTC timestamp
// ("YYYY-MM-DDTHH:MM:SSZ", Twitch's publishedAt/createdAt shape) relative to nowSeconds (a unix
// timestamp the caller supplies). Never throws: a timestamp that doesn't parse gives back "".
std::string ageText(const std::string& iso, long long nowSeconds);

}  // namespace twitch
