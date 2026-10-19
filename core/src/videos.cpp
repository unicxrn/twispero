#include "twitch/videos.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <utility>

#include "twitch/error.hpp"
#include "twitch/login.hpp"
#include "twitch/url.hpp"
#include "iso_time.hpp"
#include "json_helpers.hpp"

// Shapes verified live on 2026-10-13 against trymacs (a busy German channel) through
// twitch-cli videos/clips/vod/clip, plus a couple of ad hoc curl calls to gql.twitch.tv:
//   - user(login){videos(first,type:ARCHIVE|HIGHLIGHT|UPLOAD,sort:TIME)} and
//     user(login){clips(first,criteria:{period:LAST_DAY|...|ALL_TIME})} both answered as
//     documented in the stage 7 plan; pageInfo.hasNextPage/edges[].cursor paginate fine (a
//     real cursor came back opaque-base64-shaped, e.g. "MjA=").
//   - previewThumbnailURL(width:440,height:248) substitutes into a concrete URL
//     ("...thumb0-440x248.jpg") once Twitch has generated the thumbnail; a video still being
//     processed answers with a "vod-secure.twitch.tv/_404/404_processing_440x248.png"
//     placeholder instead (still a usable image URL, just a generic one).
//   - clip thumbnailURL with no width/height args comes back at 1920x1080 (and ignores them);
//     the CDN serves the same picture at other sizes with the size in the file name swapped
//     ("-440x248.jpg"), which the app does for its cards.
//   - a clip's broadcaster { login displayName } answered for channel and category clips alike.
//   - videoPlaybackAccessToken + the usher vod URL gave back the same shape as a live master
//     playlist (chunked/1080p60 down to 160p30, audio_only last) with #EXT-X-ENDLIST, so
//     fetchVariants/qualityMenu/pickQuality all worked unchanged.
//   - clip(slug){videoQualities{...}} gave exactly the four qualities the plan's facts
//     mention: 1080/60, 720/60, 480/30, 360/30, each sourceURL a CloudFront .mp4 behind
//     /landscape/h264/<height>/index.mp4; playbackAccessToken.value for a clip, unlike a
//     VOD's, already decodes to {"authorization":{"forbidden":false,...},"clip_slug":...}.
//   - a made-up video id and a made-up clip slug both answered with a null node (not an
//     HTTP error), which fetchVodToken/clipVariants turn into NotFoundError.

namespace twitch {
namespace {

bool isBlank(const std::string& s) {
  return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c) != 0; });
}

// Twitch rejects first > 30 on the other list endpoints (browse.cpp); the same cap applies
// here until live testing says otherwise. <= 0 means "nothing to ask for".
int clampFirst(int first) {
  if (first <= 0) return 0;
  return std::min(first, 30);
}

// ---------------------------------------------------------------------------
// GQL query text. type/period/after are baked into the query string itself
// (not sent as variables) so this never has to guess the exact enum or
// cursor scalar type names Twitch's schema uses internally - only $login and
// $first (plain String/Int) are real GraphQL variables, same as browse.cpp.
// ---------------------------------------------------------------------------

const char* videoTypeName(VideoType type) {
  switch (type) {
    case VideoType::Highlight: return "HIGHLIGHT";
    case VideoType::Upload: return "UPLOAD";
    case VideoType::Archive: return "ARCHIVE";
  }
  return "ARCHIVE";
}

const char* clipPeriodName(ClipPeriod period) {
  switch (period) {
    case ClipPeriod::Week: return "LAST_WEEK";
    case ClipPeriod::Month: return "LAST_MONTH";
    case ClipPeriod::All: return "ALL_TIME";
    case ClipPeriod::Day: return "LAST_DAY";
  }
  return "LAST_DAY";
}

// A GraphQL string literal for `s`: wraps it in double quotes, escaping '"' and '\'. Used only
// for the opaque `after` cursor Twitch itself handed back; never for user-typed text.
std::string gqlStringLiteral(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  out += '"';
  return out;
}

std::string afterArg(const std::string& after) {
  return after.empty() ? std::string() : (", after: " + gqlStringLiteral(after));
}

std::string buildChannelVideosQuery(VideoType type, const std::string& after) {
  return std::string(
             "query ChannelVideos($login: String!, $first: Int!) { user(login: $login) { "
             "videos(first: $first") +
         afterArg(after) + ", type: " + videoTypeName(type) +
         ", sort: TIME) { edges { cursor node { id title lengthSeconds viewCount "
         "previewThumbnailURL(width: 440, height: 248) publishedAt game { name } } } "
         "pageInfo { hasNextPage } } } }";
}

std::string buildChannelClipsQuery(ClipPeriod period, const std::string& after) {
  return std::string(
             "query ChannelClips($login: String!, $first: Int!) { user(login: $login) { "
             "clips(first: $first") +
         afterArg(after) + ", criteria: {period: " + clipPeriodName(period) +
         "}) { edges { cursor node { slug title viewCount durationSeconds thumbnailURL "
         "createdAt curator { displayName } game { name } broadcaster { login displayName } } } "
         "pageInfo { hasNextPage } } } }";
}

std::string buildCategoryClipsQuery(ClipPeriod period) {
  return std::string(
             "query CategoryClips($name: String!, $first: Int!) { game(name: $name) { "
             "clips(first: $first, criteria: {period: ") +
         clipPeriodName(period) +
         "}) { edges { node { slug title viewCount durationSeconds thumbnailURL createdAt "
         "curator { displayName } game { name } broadcaster { login displayName } } } } } }";
}

// A broadcast Twitch is still recording has a generic "processing" picture for a thumbnail
// (see the note at the top). The channel's live preview shows that same broadcast, at the size
// the cards draw; for a channel that has gone offline since, the CDN redirects it to its own
// offline picture.
bool isProcessingThumbnail(const std::string& url) {
  return url.find("/_404/404_processing") != std::string::npos;
}

std::string livePreviewUrl(const std::string& login) {
  return "https://static-cdn.jtvnw.net/previews-ttv/live_user_" + login + "-440x248.jpg";
}

// ---------------------------------------------------------------------------
// Node + connection parsing
// ---------------------------------------------------------------------------

VideoCard videoCardFromNode(const nlohmann::json* node) {
  VideoCard v;
  v.id = stringOr(node, "id");
  v.title = stringOr(node, "title");
  v.lengthSeconds = intOr(node, "lengthSeconds");
  v.viewCount = intOr(node, "viewCount");
  v.thumbnailUrl = stringOr(node, "previewThumbnailURL");
  v.publishedAt = stringOr(node, "publishedAt");
  v.gameName = stringOr(objectOr(node, "game"), "name");
  return v;
}

ClipCard clipCardFromNode(const nlohmann::json* node) {
  ClipCard c;
  c.slug = stringOr(node, "slug");
  c.title = stringOr(node, "title");
  c.viewCount = intOr(node, "viewCount");
  c.durationSeconds = intOr(node, "durationSeconds");
  c.thumbnailUrl = stringOr(node, "thumbnailURL");
  c.createdAt = stringOr(node, "createdAt");
  c.curatorName = stringOr(objectOr(node, "curator"), "displayName");
  c.gameName = stringOr(objectOr(node, "game"), "name");
  c.broadcasterLogin = stringOr(objectOr(node, "broadcaster"), "login");
  c.broadcasterName = stringOr(objectOr(node, "broadcaster"), "displayName");
  return c;
}

// Parses an { edges: [ {cursor, node}, ... ], pageInfo: {hasNextPage} } object found at
// data[rootKey], mapping each node with `fn`. Skips edges that aren't objects or whose node is
// missing (same as browse.cpp's parseEdges); throws ApiError when the root or its edges array
// is missing. The returned cursor is "" when pageInfo.hasNextPage is absent or false, so a
// caller never pages past the end even if Twitch still hands back a cursor on the last edge.
template <class T, class Fn>
std::pair<std::vector<T>, std::string> parsePagedEdges(const nlohmann::json& data, const char* rootKey,
                                                       const char* what, Fn fn) {
  auto root = data.find(rootKey);
  if (root == data.end() || !root->is_object()) throw ApiError(std::string(what) + " missing");
  auto edges = root->find("edges");
  if (edges == root->end() || !edges->is_array()) throw ApiError(std::string(what) + ".edges missing");

  std::vector<T> out;
  std::string lastCursor;
  for (const auto& edge : *edges) {
    if (!edge.is_object()) continue;
    const nlohmann::json* node = objectOr(&edge, "node");
    if (!node) continue;
    out.push_back(fn(node));
    const std::string cursor = stringOr(&edge, "cursor");
    if (!cursor.empty()) lastCursor = cursor;
  }

  bool hasNext = false;
  auto pageInfo = root->find("pageInfo");
  if (pageInfo != root->end() && pageInfo->is_object()) {
    auto hn = pageInfo->find("hasNextPage");
    if (hn != pageInfo->end() && hn->is_boolean()) hasNext = hn->get<bool>();
  }

  return {std::move(out), hasNext ? lastCursor : std::string()};
}

const nlohmann::json& requireUser(const nlohmann::json& data, const std::string& login) {
  auto userIt = data.find("user");
  if (userIt == data.end()) throw ApiError("user missing");
  if (userIt->is_null()) throw NotFoundError("channel not found: " + login);
  if (!userIt->is_object()) throw ApiError("user has the wrong shape");
  return *userIt;
}

// ---------------------------------------------------------------------------
// VOD token authorization check
// ---------------------------------------------------------------------------

// token.value is itself a JSON-encoded string (see playback.hpp's AccessToken comment) that,
// for a restricted VOD, carries {"authorization": {"forbidden": true, "reason": "..."}}.
// Checked up front so a subscriber-only VOD gets a clear error before ever asking usher.
void checkVodAuthorization(const AccessToken& token) {
  nlohmann::json inner = nlohmann::json::parse(token.value, nullptr, false);
  if (inner.is_discarded() || !inner.is_object()) return;  // not JSON we can inspect further
  const nlohmann::json* auth = objectOr(&inner, "authorization");
  if (!auth) return;
  auto forbiddenIt = auth->find("forbidden");
  if (forbiddenIt == auth->end() || !forbiddenIt->is_boolean() || !forbiddenIt->get<bool>()) return;
  const std::string reason = stringOr(auth, "reason");
  throw SubscriberOnlyError(reason.empty() ? "this video is for subscribers only"
                                           : "this video is for subscribers only (" + reason + ")");
}

// ageText's ISO 8601 parser is parseIso8601 (iso_time.hpp).

std::string pluralAgo(long long n, const char* unit) {
  return std::to_string(n) + " " + unit + (n == 1 ? "" : "s") + " ago";
}

int clipFps(const nlohmann::json* q) {
  if (!q || !q->is_object()) return 0;
  auto it = q->find("frameRate");
  if (it == q->end() || !it->is_number()) return 0;
  const double d = it->get<double>();
  if (d <= 0 || std::isnan(d)) return 0;
  return static_cast<int>(std::lround(d));
}

}  // namespace

VideoPage fetchChannelVideos(GqlClient& gql, const std::string& login, VideoType type, int first,
                             const std::string& after) {
  const int want = clampFirst(first);
  if (want <= 0) return {};
  const std::string normalized = normalizeLogin(login);  // throws NotFoundError, no request

  nlohmann::json data = gql.query(buildChannelVideosQuery(type, after),
                                  nlohmann::json{{"login", normalized}, {"first", want}});
  const nlohmann::json& user = requireUser(data, normalized);

  auto [cards, cursor] = parsePagedEdges<VideoCard>(user, "videos", "user.videos", videoCardFromNode);
  for (VideoCard& card : cards)
    if (isProcessingThumbnail(card.thumbnailUrl)) card.thumbnailUrl = livePreviewUrl(normalized);
  return VideoPage{std::move(cards), std::move(cursor)};
}

ClipPage fetchChannelClips(GqlClient& gql, const std::string& login, ClipPeriod period, int first,
                           const std::string& after) {
  const int want = clampFirst(first);
  if (want <= 0) return {};
  const std::string normalized = normalizeLogin(login);  // throws NotFoundError, no request

  nlohmann::json data = gql.query(buildChannelClipsQuery(period, after),
                                  nlohmann::json{{"login", normalized}, {"first", want}});
  const nlohmann::json& user = requireUser(data, normalized);

  auto [cards, cursor] = parsePagedEdges<ClipCard>(user, "clips", "user.clips", clipCardFromNode);
  return ClipPage{std::move(cards), std::move(cursor)};
}

std::vector<ClipCard> fetchCategoryClips(GqlClient& gql, const std::string& categoryName,
                                         ClipPeriod period, int first) {
  const int want = clampFirst(first);
  if (want <= 0) return {};

  nlohmann::json data = gql.query(buildCategoryClipsQuery(period),
                                  nlohmann::json{{"name", categoryName}, {"first", want}});
  auto gameIt = data.find("game");
  if (gameIt == data.end()) throw ApiError("game missing");
  if (gameIt->is_null()) throw NotFoundError("category not found: " + categoryName);
  if (!gameIt->is_object()) throw ApiError("game has the wrong shape");

  auto [cards, cursor] = parsePagedEdges<ClipCard>(*gameIt, "clips", "game.clips", clipCardFromNode);
  (void)cursor;  // fetchCategoryClips doesn't paginate
  return cards;
}

const char* const kVodPlaybackTokenQuery =
    "query VodPlaybackAccessToken($id: ID!) { videoPlaybackAccessToken(id: $id, params: "
    "{platform: \"web\", playerBackend: \"mediaplayer\", playerType: \"site\"}) { value "
    "signature } }";

AccessToken fetchVodToken(GqlClient& gql, const std::string& videoId) {
  if (isBlank(videoId)) throw NotFoundError("video not found: " + videoId);
  nlohmann::json data = gql.query(kVodPlaybackTokenQuery, nlohmann::json{{"id", videoId}});
  if (!data.is_object()) throw ApiError("videoPlaybackAccessToken missing");
  auto it = data.find("videoPlaybackAccessToken");
  if (it == data.end()) throw ApiError("videoPlaybackAccessToken missing");
  if (it->is_null()) throw NotFoundError("video not found: " + videoId);
  const auto& t = *it;
  if (!t.is_object() || !t.contains("value") || !t["value"].is_string() ||
      !t.contains("signature") || !t["signature"].is_string())
    throw ApiError("videoPlaybackAccessToken has an unexpected shape");

  AccessToken token{t["value"].get<std::string>(), t["signature"].get<std::string>()};
  checkVodAuthorization(token);  // throws SubscriberOnlyError when forbidden
  return token;
}

std::string vodPlaylistUrl(const std::string& videoId, const AccessToken& token) {
  return std::string(kUsherVodBase) + percentEncode(videoId) + ".m3u8?" +
         buildQuery({
             {"allow_audio_only", "true"},
             {"allow_source", "true"},
             {"sig", token.signature},
             {"supported_codecs", "avc1"},
             {"token", token.value},
         });
}

std::vector<Variant> resolveVod(GqlClient& gql, HttpClient& http, const std::string& videoId) {
  const AccessToken token = fetchVodToken(gql, videoId);  // NotFoundError / SubscriberOnlyError
  const std::string url = vodPlaylistUrl(videoId, token);
  try {
    return fetchVariants(http, url);  // fetch + parseMaster, same call the live path makes
  } catch (const HttpStatusError& e) {
    if (e.status() == 403) throw SubscriberOnlyError("this video is for subscribers only");
    throw;
  } catch (const OfflineError&) {
    // fetchVariants maps a 404 to "channel is offline", which makes no sense for a VOD.
    throw NotFoundError("video not found: " + videoId);
  }
}

const char* const kClipVariantsQuery =
    "query ClipVariants($slug: ID!) { clip(slug: $slug) { videoQualities { quality frameRate "
    "sourceURL } playbackAccessToken(params: {platform: \"web\", playerBackend: \"mediaplayer\", "
    "playerType: \"site\"}) { value signature } } }";

std::vector<ClipVariant> clipVariants(GqlClient& gql, const std::string& slug) {
  if (isBlank(slug)) throw NotFoundError("clip not found: " + slug);
  nlohmann::json data = gql.query(kClipVariantsQuery, nlohmann::json{{"slug", slug}});
  if (!data.is_object()) throw ApiError("clip missing");
  auto it = data.find("clip");
  if (it == data.end()) throw ApiError("clip missing");
  if (it->is_null()) throw NotFoundError("clip not found: " + slug);  // also a deleted clip
  const auto& clip = *it;
  if (!clip.is_object()) throw ApiError("clip has the wrong shape");

  const nlohmann::json* tokenObj = objectOr(&clip, "playbackAccessToken");
  if (!tokenObj || !tokenObj->contains("value") || !(*tokenObj)["value"].is_string() ||
      !tokenObj->contains("signature") || !(*tokenObj)["signature"].is_string())
    throw ApiError("clip.playbackAccessToken has an unexpected shape");
  const std::string sig = (*tokenObj)["signature"].get<std::string>();
  const std::string value = (*tokenObj)["value"].get<std::string>();

  auto qIt = clip.find("videoQualities");
  if (qIt == clip.end() || !qIt->is_array()) throw ApiError("clip.videoQualities missing");

  std::vector<ClipVariant> out;
  for (const auto& q : *qIt) {
    if (!q.is_object()) continue;
    const std::string sourceUrl = stringOr(&q, "sourceURL");
    if (sourceUrl.empty()) continue;
    ClipVariant v;
    v.quality = stringOr(&q, "quality");
    v.fps = clipFps(&q);
    v.url = sourceUrl + "?" + buildQuery({{"sig", sig}, {"token", value}});
    out.push_back(std::move(v));
  }
  return out;
}

ClipVariant pickClipQuality(const std::vector<ClipVariant>& variants, const std::string& wanted) {
  if (variants.empty()) throw ApiError("no clip variants");

  // Reuses pickQuality's ranking by wrapping each ClipVariant as a Variant (height/fps/name/
  // label filled in, bandwidth and source left at their defaults - clips don't have either),
  // stashing the original index in `uri` so the chosen Variant maps straight back.
  std::vector<Variant> asVariants;
  asVariants.reserve(variants.size());
  for (size_t i = 0; i < variants.size(); ++i) {
    Variant v;
    v.height = std::atoi(variants[i].quality.c_str());
    v.fps = variants[i].fps;
    v.name = variants[i].quality;
    v.label = variants[i].quality + "p" + (variants[i].fps >= 45 ? "60" : "");
    v.uri = std::to_string(i);
    asVariants.push_back(std::move(v));
  }

  const Variant chosen = pickQuality(asVariants, wanted);
  return variants[static_cast<size_t>(std::strtoul(chosen.uri.c_str(), nullptr, 10))];
}

std::string ageText(const std::string& iso, long long nowSeconds) {
  long long then = 0;
  if (!parseIso8601(iso, &then)) return "";
  long long diff = nowSeconds - then;
  if (diff < 0) diff = 0;

  if (diff < 60) return "just now";
  if (diff < 3600) return pluralAgo(diff / 60, "minute");
  if (diff < 86400) return pluralAgo(diff / 3600, "hour");
  if (diff < 86400LL * 30) return pluralAgo(diff / 86400, "day");
  if (diff < 86400LL * 365) return pluralAgo(diff / (86400 * 30), "month");
  return pluralAgo(diff / (86400LL * 365), "year");
}

}  // namespace twitch
