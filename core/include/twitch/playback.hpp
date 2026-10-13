#pragma once
#include <string>
#include <vector>

#include "twitch/gql.hpp"
#include "twitch/hls.hpp"
#include "twitch/http.hpp"

namespace twitch {

struct AccessToken {
  std::string value;      // a JSON string; passed through unchanged
  std::string signature;
};

extern const char* const kPlaybackTokenQuery;
inline constexpr const char* kUsherStreamBase = "https://usher.ttvnw.net/api/channel/hls/";

// playerType "site" is the normal web player; "embed", "popout", "autoplay" are used as
// ad-free backups (see spec section 4). Throws NotFoundError if the channel does not exist or
// if the login is invalid. Throws ApiError if the token is missing or malformed.
AccessToken fetchStreamToken(GqlClient& gql, const std::string& login,
                             const std::string& playerType = "site");

// Master playlist URL for a live channel. Normalizes the login (lowercase) and throws
// NotFoundError for an invalid channel name. nonce is the "p" parameter (any random number).
std::string usherStreamUrl(const std::string& login, const AccessToken& token, unsigned nonce);

// Downloads and parses the master playlist. Throws OfflineError on HTTP 404,
// ApiError on other non-200 statuses.
std::vector<Variant> fetchVariants(HttpClient& http, const std::string& masterUrl);

// preferred: "best" (highest bandwidth), "worst" (lowest), "source" (the source-quality
// rendition, falling back to "best" if none is marked), an exact name ("chunked", "720p60"),
// an exact label ("720p"), or a label with a trailing " (source)" removed ("1080p60" matches
// a variant labeled "1080p60 (source)"). These are tried in that order — exact name first,
// then label — as separate passes over all variants, so an exact name match always wins even
// if an earlier variant in the list only matches by label. Anything else falls back to "best".
// "best", "worst" and the unknown-name fallback skip audio-only variants (height 0) unless
// only audio-only variants exist. Throws ApiError if variants is empty. Returns a copy, so
// it is safe to call on a temporary vector.
Variant pickVariant(const std::vector<Variant>& variants, const std::string& preferred);

// The variant to play for a quality the user asked for: a Settings value (kQualities in
// settings.hpp) or a name from qualityMenu(). "Best" is the top of qualityMenu()'s ordering
// (taller, then faster, then more bandwidth; video before audio only), so the first pick and
// the picker's first row agree even when the source has the lower bitrate.
//   - "auto" (and "") is the best, "source" the source rendition (else the best);
//   - a variant whose name or label is `wanted` ("720p60", "chunked", "audio_only", or
//     "1080p60" for a variant labelled "1080p60 (source)") is that variant;
//   - otherwise a name like "720p60" or "480p" (no frame rate means 30) takes the closest video
//     variant: the best one at or below that height, at or below that frame rate at the same
//     height ("720p" never takes 720p60); when there is none, the lowest one above it;
//   - a name that means nothing takes the best.
// Throws ApiError if variants is empty.
Variant pickQuality(const std::vector<Variant>& variants, const std::string& wanted);

// What the quality picker calls a variant: Twitch's label ("1080p60 (source)", "720p60",
// "480p"), "Audio only" for the audio rendition, and the size and frame rate ("1080p60") when
// the master gave the variant no name of its own. The source rendition always says so.
std::string qualityLabel(const Variant& variant);

// One row of the quality picker.
struct QualityOption {
  std::string name;   // the variant's name, for pickQuality()
  std::string label;  // qualityLabel()
  int width = 0;      // 0 for audio only
  int height = 0;
  int fps = 0;        // rounded; 0 when the master doesn't say
};

// The variants as the picker lists them: video best first (taller, then faster, then more
// bandwidth), audio only last, each name once.
std::vector<QualityOption> qualityMenu(const std::vector<Variant>& variants);

}  // namespace twitch
