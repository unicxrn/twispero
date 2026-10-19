#include "twitch/browse.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <utility>

#include "twitch/error.hpp"
#include "twitch/login.hpp"
#include "iso_time.hpp"
#include "json_helpers.hpp"

namespace twitch {
namespace {

const char* const kTopCategoriesQuery =
    "query TopCategories($first: Int!) { games(first: $first) { edges { node { "
    "id name displayName viewersCount boxArtURL(width: 285, height: 380) } } } }";

// In two halves, split where a language filter goes (categoryStreams).
const char* const kCategoryStreamsHead =
    "query CategoryStreams($name: String!, $first: Int!) { game(name: $name) { "
    "name streams(first: $first";
const char* const kCategoryStreamsTail =
    ") { edges { node { id title viewersCount "
    "previewImageURL(width: 440, height: 248) broadcaster { login displayName } } } } } }";

const char* const kSearchCategoriesQuery =
    "query SearchCategories($query: String!, $first: Int!) { searchCategories(query: $query, "
    "first: $first) { edges { node { id name boxArtURL(width: 144, height: 192) viewersCount } } } }";

const char* const kSearchChannelsQuery =
    "query SearchChannels($query: String!, $first: Int!) { searchUsers(userQuery: $query, "
    "first: $first) { edges { node { login displayName profileImageURL(width: 70) "
    "stream { viewersCount } } } } }";

// stream { createdAt language freeformTags { name } } and channel(name){schedule{segments}}
// verified live and anonymous on 2026-10-13 (a busy esports channel and two that set a
// schedule): createdAt "2026-10-13T05:41:34Z", language "EN" (upper case), freeformTags
// [{"name":"English"},{"name":"LPL"},...]; schedule null for a channel without one;
// segments (no arguments: "first" is refused) lists the current week, past segments
// included, each {startAt, endAt (null when open-ended), title ("" when none), isCancelled,
// categories [{name}] (possibly empty)}.
const char* const kChannelQuery =
    "query Channel($login: String!) { user(login: $login) { id login displayName description "
    "profileImageURL(width: 150) followers { totalCount } stream { id title viewersCount "
    "createdAt language freeformTags { name } game { name } } } }";

const char* const kScheduleQuery =
    "query Schedule($login: String!) { channel(name: $login) { schedule { segments { "
    "startAt endAt title isCancelled categories { name } } } } }";

// Twitch's broadcaster languages, by the code before any region ("zh-hk").
constexpr const char* kLanguageNames[][2] = {
    {"ar", "Arabic"},     {"bg", "Bulgarian"},  {"ca", "Catalan"},    {"cs", "Czech"},
    {"da", "Danish"},     {"de", "German"},     {"el", "Greek"},      {"en", "English"},
    {"es", "Spanish"},    {"fi", "Finnish"},    {"fr", "French"},     {"he", "Hebrew"},
    {"hi", "Hindi"},      {"hu", "Hungarian"},  {"id", "Indonesian"}, {"it", "Italian"},
    {"ja", "Japanese"},   {"ko", "Korean"},     {"ms", "Malay"},      {"nl", "Dutch"},
    {"no", "Norwegian"},  {"pl", "Polish"},     {"pt", "Portuguese"}, {"ro", "Romanian"},
    {"ru", "Russian"},    {"sk", "Slovak"},     {"sv", "Swedish"},    {"th", "Thai"},
    {"tl", "Tagalog"},    {"tr", "Turkish"},    {"uk", "Ukrainian"},  {"vi", "Vietnamese"},
    {"zh", "Chinese"},    {"asl", "Sign language"},
};

// Twitch rejects first > 30; <= 0 means "nothing to ask for".
int clampFirst(int first) {
  if (first <= 0) return 0;
  return std::min(first, 30);
}

bool isBlank(const std::string& s) {
  return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c) != 0; });
}

// True when url's scheme and host (checked case-insensitively) are
// http(s)://static-cdn.jtvnw.net/, followed by a (case-sensitive) path
// starting with previews-ttv/. Anchored at position 0 so a URL that merely
// mentions that host somewhere in its own query string - e.g. a redirector
// or an attacker-controlled URL - never matches.
bool isLivePreviewUrl(const std::string& url) {
  auto startsWithCi = [&](const char* prefix) {
    size_t n = std::strlen(prefix);
    if (url.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
      if (std::tolower(static_cast<unsigned char>(url[i])) !=
          std::tolower(static_cast<unsigned char>(prefix[i])))
        return false;
    }
    return true;
  };

  size_t hostEnd = 0;
  if (startsWithCi("https://static-cdn.jtvnw.net/")) {
    hostEnd = std::strlen("https://static-cdn.jtvnw.net/");
  } else if (startsWithCi("http://static-cdn.jtvnw.net/")) {
    hostEnd = std::strlen("http://static-cdn.jtvnw.net/");
  } else {
    return false;
  }
  const std::string pathPrefix = "previews-ttv/";
  return url.compare(hostEnd, pathPrefix.size(), pathPrefix) == 0;
}

// True when query[pos, pos+1] is exactly the parameter name "t" followed by
// '=' (i.e. it is a "t=..." parameter, not e.g. "test=...").
bool isTParam(const std::string& query, size_t pos) {
  return pos + 1 < query.size() && query[pos] == 't' && query[pos + 1] == '=';
}

CategoryInfo categoryFromNode(const nlohmann::json* node) {
  CategoryInfo c;
  c.id = stringOr(node, "id");
  c.name = stringOr(node, "name");
  c.boxArtUrl = stringOr(node, "boxArtURL");
  c.viewers = intOrDefault(node, "viewersCount", -1);
  return c;
}

// Parses an { edges: [ {node: ...}, ... ] } object found at data[rootKey],
// mapping each node with `fn`. Skips edges that aren't objects or whose node
// is missing; throws ApiError when the root or its edges array is missing.
template <class T, class Fn>
std::vector<T> parseEdges(const nlohmann::json& data, const char* rootKey, const char* what, Fn fn) {
  auto root = data.find(rootKey);
  if (root == data.end() || !root->is_object()) throw ApiError(std::string(what) + " missing");
  auto edges = root->find("edges");
  if (edges == root->end() || !edges->is_array()) throw ApiError(std::string(what) + ".edges missing");

  std::vector<T> out;
  for (const auto& edge : *edges) {
    const nlohmann::json* node = objectOr(&edge, "node");
    if (!node) continue;
    out.push_back(fn(node));
  }
  return out;
}

}  // namespace

std::vector<CategoryInfo> parseTopCategories(const nlohmann::json& data) {
  return parseEdges<CategoryInfo>(data, "games", "games",
                                   [](const nlohmann::json* node) { return categoryFromNode(node); });
}

// Twitch returns exactly the number of categories asked for (no off-by-one,
// unlike streams), but we still trim defensively in case that changes.
std::vector<CategoryInfo> topCategories(GqlClient& gql, int first) {
  const int want = clampFirst(first);
  if (want <= 0) return {};
  std::vector<CategoryInfo> cats =
      parseTopCategories(gql.query(kTopCategoriesQuery, nlohmann::json{{"first", want}}));
  if (cats.size() > static_cast<size_t>(want)) cats.resize(static_cast<size_t>(want));
  return cats;
}

std::vector<StreamInfo> parseCategoryStreams(const nlohmann::json& data, const std::string& categoryName) {
  auto gameIt = data.find("game");
  if (gameIt == data.end()) throw ApiError("game missing");
  if (gameIt->is_null()) throw NotFoundError("category not found: " + categoryName);
  if (!gameIt->is_object()) throw ApiError("game has the wrong shape");
  const nlohmann::json& game = *gameIt;

  std::string gameName = stringOr(&game, "name");
  if (gameName.empty()) gameName = categoryName;

  auto streamsIt = game.find("streams");
  if (streamsIt == game.end() || !streamsIt->is_object()) throw ApiError("game.streams missing");
  auto edges = streamsIt->find("edges");
  if (edges == streamsIt->end() || !edges->is_array()) throw ApiError("game.streams.edges missing");

  std::vector<StreamInfo> out;
  for (const auto& edge : *edges) {
    const nlohmann::json* node = objectOr(&edge, "node");
    if (!node) continue;
    const nlohmann::json* broadcaster = objectOr(node, "broadcaster");
    if (!broadcaster) continue;

    StreamInfo s;
    s.id = stringOr(node, "id");
    s.title = stringOr(node, "title");
    s.viewers = intOr(node, "viewersCount");
    s.previewUrl = stringOr(node, "previewImageURL");
    s.login = stringOr(broadcaster, "login");
    s.displayName = stringOr(broadcaster, "displayName");
    s.gameName = gameName;
    out.push_back(std::move(s));
  }
  return out;
}

// Same quirk as topStreams: streams(first: n) returns n-1 edges.
std::vector<StreamInfo> categoryStreams(GqlClient& gql, const std::string& categoryName, int first,
                                        const std::string& language) {
  const int want = clampFirst(first);
  if (want <= 0) return {};
  const int ask = std::min(want, 25) + 5;
  // A language goes in as an enum, never as a variable (see topStreams).
  const std::string query = kCategoryStreamsHead + browseLanguageOption(language) + kCategoryStreamsTail;
  std::vector<StreamInfo> streams = parseCategoryStreams(
      gql.query(query, nlohmann::json{{"name", categoryName}, {"first", ask}}), categoryName);
  if (streams.size() > static_cast<size_t>(want)) streams.resize(static_cast<size_t>(want));
  return streams;
}

std::vector<CategoryInfo> parseSearchCategories(const nlohmann::json& data) {
  return parseEdges<CategoryInfo>(data, "searchCategories", "searchCategories",
                                   [](const nlohmann::json* node) { return categoryFromNode(node); });
}

std::vector<CategoryInfo> searchCategories(GqlClient& gql, const std::string& query, int first) {
  if (isBlank(query)) return {};
  const int want = clampFirst(first);
  if (want <= 0) return {};
  std::vector<CategoryInfo> cats = parseSearchCategories(
      gql.query(kSearchCategoriesQuery, nlohmann::json{{"query", query}, {"first", want}}));
  if (cats.size() > static_cast<size_t>(want)) cats.resize(static_cast<size_t>(want));
  return cats;
}

std::vector<ChannelHit> parseSearchChannels(const nlohmann::json& data) {
  return parseEdges<ChannelHit>(data, "searchUsers", "searchUsers", [](const nlohmann::json* node) {
    ChannelHit h;
    h.login = stringOr(node, "login");
    h.displayName = stringOr(node, "displayName");
    h.profileImageUrl = stringOr(node, "profileImageURL");
    const nlohmann::json* stream = objectOr(node, "stream");
    h.liveViewers = stream ? intOr(stream, "viewersCount") : -1;
    return h;
  });
}

std::vector<ChannelHit> searchChannels(GqlClient& gql, const std::string& query, int first) {
  if (isBlank(query)) return {};
  const int want = clampFirst(first);
  if (want <= 0) return {};
  std::vector<ChannelHit> hits = parseSearchChannels(
      gql.query(kSearchChannelsQuery, nlohmann::json{{"query", query}, {"first", want}}));
  if (hits.size() > static_cast<size_t>(want)) hits.resize(static_cast<size_t>(want));
  return hits;
}

ChannelInfo parseChannel(const nlohmann::json& data, const std::string& login) {
  auto userIt = data.find("user");
  if (userIt == data.end()) throw ApiError("user missing");
  if (userIt->is_null()) throw NotFoundError("channel not found: " + login);
  if (!userIt->is_object()) throw ApiError("user has the wrong shape");
  const nlohmann::json& user = *userIt;

  ChannelInfo info;
  info.id = stringOr(&user, "id");
  info.login = stringOr(&user, "login");
  info.displayName = stringOr(&user, "displayName");
  info.description = stringOr(&user, "description");
  info.profileImageUrl = stringOr(&user, "profileImageURL");
  info.followers = intOrDefault(objectOr(&user, "followers"), "totalCount", -1);

  const nlohmann::json* stream = objectOr(&user, "stream");
  info.live = stream != nullptr;
  if (stream) {
    info.streamTitle = stringOr(stream, "title");
    info.viewers = intOr(stream, "viewersCount");
    info.gameName = stringOr(objectOr(stream, "game"), "name");
    info.streamStartedAt = stringOr(stream, "createdAt");
    info.language = stringOr(stream, "language");
    const auto tags = stream->find("freeformTags");
    if (tags != stream->end() && tags->is_array()) {
      for (const auto& tag : *tags) {
        if (static_cast<int>(info.tags.size()) >= kMaxStreamTags) break;
        std::string name = stringOr(&tag, "name");
        if (!isBlank(name)) info.tags.push_back(std::move(name));
      }
    }
  }
  return info;
}

ChannelInfo fetchChannel(GqlClient& gql, const std::string& login) {
  const std::string normalized = normalizeLogin(login);  // throws NotFoundError, no request
  nlohmann::json data = gql.query(kChannelQuery, nlohmann::json{{"login", normalized}});
  return parseChannel(data, normalized);
}

std::vector<ScheduleSegment> parseSchedule(const nlohmann::json& data, const std::string& login,
                                           long long nowSeconds, int max) {
  auto channelIt = data.find("channel");
  if (channelIt == data.end()) throw ApiError("channel missing");
  if (channelIt->is_null()) throw NotFoundError("channel not found: " + login);
  if (!channelIt->is_object()) throw ApiError("channel has the wrong shape");
  std::vector<ScheduleSegment> out;
  if (max <= 0) return out;
  // No schedule at all is an answer too: nothing planned.
  const nlohmann::json* schedule = objectOr(&*channelIt, "schedule");
  if (!schedule) return out;
  auto segments = schedule->find("segments");
  if (segments == schedule->end() || segments->is_null()) return out;
  if (!segments->is_array()) throw ApiError("schedule.segments has the wrong shape");

  std::vector<std::pair<long long, ScheduleSegment>> upcoming;
  for (const auto& seg : *segments) {
    if (!seg.is_object()) continue;
    const auto cancelled = seg.find("isCancelled");
    if (cancelled != seg.end() && cancelled->is_boolean() && cancelled->get<bool>()) continue;
    ScheduleSegment s;
    s.startAt = stringOr(&seg, "startAt");
    long long start = 0;
    if (!parseIso8601(s.startAt, &start) || start <= nowSeconds) continue;
    s.endAt = stringOr(&seg, "endAt");
    s.title = stringOr(&seg, "title");
    const auto cats = seg.find("categories");
    if (cats != seg.end() && cats->is_array()) {
      for (const auto& c : *cats) {
        s.category = stringOr(&c, "name");
        if (!s.category.empty()) break;
      }
    }
    upcoming.emplace_back(start, std::move(s));
  }
  std::stable_sort(upcoming.begin(), upcoming.end(),
                   [](const auto& a, const auto& b) { return a.first < b.first; });
  for (auto& u : upcoming) {
    if (static_cast<int>(out.size()) >= max) break;
    out.push_back(std::move(u.second));
  }
  return out;
}

std::vector<ScheduleSegment> fetchSchedule(GqlClient& gql, const std::string& login, long long nowSeconds,
                                           int max) {
  const std::string normalized = normalizeLogin(login);  // throws NotFoundError, no request
  return parseSchedule(gql.query(kScheduleQuery, nlohmann::json{{"login", normalized}}), normalized,
                       nowSeconds, max);
}

std::string uptimeText(const std::string& startedAt, long long nowSeconds) {
  long long start = 0;
  if (!parseIso8601(startedAt, &start)) return "";
  const long long on = nowSeconds - start;
  if (on < -300) return "";
  if (on < 60) return "Just went live";
  const long long hours = on / 3600, minutes = on % 3600 / 60;
  if (hours == 0) return "Live for " + std::to_string(minutes) + " min";
  if (minutes == 0) return "Live for " + std::to_string(hours) + " h";
  return "Live for " + std::to_string(hours) + " h " + std::to_string(minutes) + " min";
}

std::string startsInText(const std::string& startAt, long long nowSeconds) {
  long long start = 0;
  if (!parseIso8601(startAt, &start)) return "";
  const long long in = start - nowSeconds;
  if (in < 30) return "Starting now";
  // To the nearest minute, hour or day, never less than one.
  const long long minutes = std::max(1LL, (in + 30) / 60);
  if (minutes < 60) return "In " + std::to_string(minutes) + " min";
  const long long hours = (in + 1800) / 3600;
  if (hours < 24) return "In " + std::to_string(hours) + " h";
  const long long days = (in + 43200) / 86400;
  return "In " + std::to_string(days) + (days == 1 ? " day" : " days");
}

bool isBrowseLanguage(const std::string& code) noexcept {
  if (code.empty()) return false;
  for (int i = 1; i < kBrowseLanguageCount; ++i)
    if (code == kBrowseLanguages[i]) return true;
  return false;
}

std::string browseLanguageOption(const std::string& code) {
  if (!isBrowseLanguage(code)) return "";
  return ", options: {broadcasterLanguages: [" + code + "]}";
}

static int browseLanguageIndex(const std::string& code) noexcept {
  for (int i = 0; i < kBrowseLanguageCount; ++i)
    if (code == kBrowseLanguages[i]) return i;
  return 0;
}

std::string nextBrowseLanguage(const std::string& code) {
  const int i = browseLanguageIndex(code);
  return kBrowseLanguages[(i + 1) % kBrowseLanguageCount];
}

std::string prevBrowseLanguage(const std::string& code) {
  const int i = browseLanguageIndex(code);
  return kBrowseLanguages[(i + kBrowseLanguageCount - 1) % kBrowseLanguageCount];
}

std::string browseLanguageLabel(const std::string& code) {
  if (!isBrowseLanguage(code)) return "All languages";
  const std::string name = languageName(code);
  return name.empty() ? code : name;
}

std::string languageName(const std::string& code) {
  std::string key;
  for (const char c : code) {
    if (c == '-' || c == '_') break;
    key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  for (const auto& n : kLanguageNames)
    if (key == n[0]) return n[1];
  return "";
}

// Adding, replacing or leaving a query parameter is all string surgery below
// the '?'/'#': split the URL into base + query + fragment, find an existing
// "t=..." parameter inside the query (bounded by '&' or the query's end),
// and either replace its value in place or append a new "t=" parameter.
std::string liveThumbnailUrl(const std::string& url, long long nowSeconds, int bucketSeconds) {
  if (bucketSeconds <= 0) return url;
  if (!isLivePreviewUrl(url)) return url;

  const long long bucket = nowSeconds / bucketSeconds;
  const std::string value = std::to_string(bucket);

  const size_t fragPos = url.find('#');
  const std::string base = url.substr(0, fragPos);
  const std::string fragment = (fragPos == std::string::npos) ? std::string() : url.substr(fragPos);

  const size_t queryPos = base.find('?');
  if (queryPos == std::string::npos) return base + "?t=" + value + fragment;

  const std::string query = base.substr(queryPos + 1);
  size_t pos = 0;
  while (pos <= query.size()) {
    const size_t amp = query.find('&', pos);
    const size_t end = (amp == std::string::npos) ? query.size() : amp;
    if (isTParam(query, pos)) {
      const std::string newQuery = query.substr(0, pos) + "t=" + value + query.substr(end);
      return base.substr(0, queryPos + 1) + newQuery + fragment;
    }
    if (amp == std::string::npos) break;
    pos = amp + 1;
  }

  return base + "&t=" + value + fragment;
}

}  // namespace twitch
