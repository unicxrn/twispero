#pragma once
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "twitch/gql.hpp"
#include "twitch/streams.hpp"

namespace twitch {

struct CategoryInfo {
  std::string id;
  std::string name;
  std::string boxArtUrl;  // 285x380 (top list) or 144x192 (search)
  int viewers = -1;       // -1 when Twitch doesn't say
};

struct ChannelHit {
  std::string login;
  std::string displayName;
  std::string profileImageUrl;  // 70x70, JPEG or PNG
  int liveViewers = -1;         // -1 when offline
};

struct ChannelInfo {
  std::string id, login, displayName, description, profileImageUrl;  // 150x150
  int followers = -1;
  bool live = false;
  std::string streamTitle, gameName;
  int viewers = -1;
  // While live (stage 7): when the stream started, ISO 8601 as Twitch sends it
  // (see uptimeText); the tags the streamer set, in their order (at most
  // kMaxStreamTags, none empty); the stream's language code ("EN", "DE"; see
  // languageName), "" when Twitch doesn't say.
  std::string streamStartedAt;
  std::vector<std::string> tags;
  std::string language;
};

inline constexpr int kMaxStreamTags = 10;  // Twitch's own limit for a stream

// One stream on a channel's schedule.
struct ScheduleSegment {
  std::string startAt;   // ISO 8601 UTC, as Twitch sends it (see startsInText)
  std::string endAt;     // "" when it has no set end
  std::string title;     // "" when the streamer gave none
  std::string category;  // the first category, "" when none
};

std::vector<CategoryInfo> parseTopCategories(const nlohmann::json& data);
std::vector<CategoryInfo> topCategories(GqlClient& gql, int first);

// Streams in one category, most viewers first. Asks Twitch for a few extra (same quirk as topStreams).
// Throws NotFoundError when the category doesn't exist. `language` filters to one broadcaster
// language (see kBrowseLanguages); "" and anything not on that list ask for every language.
std::vector<StreamInfo> parseCategoryStreams(const nlohmann::json& data, const std::string& categoryName);
std::vector<StreamInfo> categoryStreams(GqlClient& gql, const std::string& categoryName, int first,
                                        const std::string& language = {});

std::vector<CategoryInfo> parseSearchCategories(const nlohmann::json& data);
std::vector<CategoryInfo> searchCategories(GqlClient& gql, const std::string& query, int first);
std::vector<ChannelHit> parseSearchChannels(const nlohmann::json& data);
std::vector<ChannelHit> searchChannels(GqlClient& gql, const std::string& query, int first);

// Throws NotFoundError when the user doesn't exist (also for invalid logins, via normalizeLogin).
ChannelInfo parseChannel(const nlohmann::json& data, const std::string& login);
ChannelInfo fetchChannel(GqlClient& gql, const std::string& login);

// A channel's next scheduled streams (anonymous GQL: channel(name){schedule{segments}}, which
// lists about a week of them, past ones included): the ones that start after nowSeconds and
// aren't cancelled, soonest first, at most max. A channel with no schedule gives an empty
// list. parseSchedule throws NotFoundError when the channel doesn't exist and ApiError when
// the answer has another shape; fetchSchedule throws NotFoundError for an invalid login too,
// with no request sent.
std::vector<ScheduleSegment> parseSchedule(const nlohmann::json& data, const std::string& login,
                                           long long nowSeconds, int max);
std::vector<ScheduleSegment> fetchSchedule(GqlClient& gql, const std::string& login, long long nowSeconds,
                                           int max);

// How long a stream that started at startedAt (ISO 8601 UTC) has been on as of nowSeconds:
// "Live for 2 h 14 min", "Live for 3 h", "Live for 45 min", or "Just went live" in its first
// minute. "" when the time doesn't parse or is more than 5 minutes ahead (a console clock far
// behind). Never throws.
std::string uptimeText(const std::string& startedAt, long long nowSeconds);

// When a scheduled stream starts, as of nowSeconds: "In 45 min", "In 5 h", "In 1 day",
// "In 3 days" (to the nearest minute, hour or day; Twitch's times are UTC and the console's
// time zone isn't known, so never a clock time). "Starting now" once it is due, "" when the
// time doesn't parse. Never throws.
std::string startsInText(const std::string& startAt, long long nowSeconds);

// The English name of a stream language code as Twitch sends it ("EN" -> "English",
// "zh-hk" -> "Chinese"), any case; "" for a code not in the list ("OTHER" included).
std::string languageName(const std::string& code);

// Browse's language filter. The first entry is "" (every language). The rest are
// Twitch Language enum values, unquoted in a GQL query. "All languages" is the
// label for "". next/prev wrap. A code not in the list is treated as "".
inline constexpr const char* kBrowseLanguages[] = {
    "",  "EN", "ES", "PT", "DE", "FR", "IT", "NL", "PL",
    "RU", "UK", "JA", "KO", "ZH", "TR", "AR", "TH", "SV"};
inline constexpr int kBrowseLanguageCount =
    static_cast<int>(sizeof kBrowseLanguages / sizeof kBrowseLanguages[0]);

bool isBrowseLanguage(const std::string& code) noexcept;
// The GQL options clause for a whitelisted code (", options: {broadcasterLanguages: [DE]}"),
// or "" for every language and for a code that is not on the list.
std::string browseLanguageOption(const std::string& code);
std::string nextBrowseLanguage(const std::string& code);
std::string prevBrowseLanguage(const std::string& code);
// "All languages", or languageName() for a code on the list.
std::string browseLanguageLabel(const std::string& code);

// Adds (or, if one is already there, replaces) a `t=<bucket>` query parameter
// on a live preview URL so caches refresh; the bucket is nowSeconds / bucketSeconds,
// so calling this again with the same bucket gives back the same URL
// (idempotent) and a later bucket overwrites the old value in place, leaving
// any other query parameters untouched. A `#fragment`, if present, stays
// after the parameter. Only rewrites a URL whose scheme and host (checked
// case-insensitively) are http(s)://static-cdn.jtvnw.net/ with a path
// starting with previews-ttv/; every other URL - including one that merely
// mentions that host somewhere in its own query string - is returned
// unchanged. bucketSeconds > 0.
std::string liveThumbnailUrl(const std::string& url, long long nowSeconds, int bucketSeconds = 300);

}  // namespace twitch
