// Checked live on 2026-10-12 with the app's own Client ID and a real login
// (twitch-cli login / me / following / say): /users, /streams/followed,
// /channels/followed and POST /chat/messages answer with the field names used
// below, and a message to the owner's channel came back with is_sent true.
//
// POST /clips (createClip) is not checked live: making a real clip needs a
// channel that is live and a login with clips:edit. Its shape is Twitch's
// documented one (dev.twitch.tv/docs/api/reference/#create-clip): 202 with
// {"data":[{"id":"<slug>","edit_url":"https://clips.twitch.tv/<slug>/edit"}]},
// 403 when the channel doesn't allow clips, 404 when it isn't live.

#include "twitch/helix.hpp"

#include <algorithm>
#include <cctype>

#include <nlohmann/json.hpp>

#include "json_helpers.hpp"
#include "twitch/browse.hpp"
#include "twitch/error.hpp"
#include "twitch/login.hpp"
#include "twitch/url.hpp"

namespace twitch {
namespace {

nlohmann::json parseBody(const HttpResponse& res) {
  return nlohmann::json::parse(res.body, nullptr, /*allow_exceptions=*/false);
}

// Twitch's own words for a 401 that is about a missing scope ("Missing
// scope: user:read:follows"), or "" for any other 401 (a refused token says
// "Invalid OAuth token"). Scope names are all it can carry, never a token.
std::string missingScopeMessage(const HttpResponse& res) {
  const nlohmann::json body = parseBody(res);
  std::string message = stringOr(&body, "message");
  std::string lower = message;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (lower.find("scope") == std::string::npos) return std::string();
  if (message.size() > 100) message.resize(100);
  return message;
}

// 401 -> UnauthorizedError (missingScope() when Twitch says a scope is
// missing); any other status outside [200, 300) (including a 3xx) ->
// HttpStatusError. Never includes the token: callers only pass `what`, a
// short fixed label.
void checkStatus(const HttpResponse& res, const char* what) {
  if (res.status == 401) {
    const std::string scope = missingScopeMessage(res);
    if (!scope.empty()) throw UnauthorizedError(std::string(what) + " HTTP 401: " + scope, true);
    throw UnauthorizedError(std::string(what) + " HTTP 401");
  }
  if (res.status < 200 || res.status >= 300)
    throw HttpStatusError(std::string(what) + " HTTP " + std::to_string(res.status), res.status);
}

bool boolOr(const nlohmann::json* obj, const char* key, bool def) {
  if (!obj || !obj->is_object()) return def;
  auto it = obj->find(key);
  if (it == obj->end() || !it->is_boolean()) return def;
  return it->get<bool>();
}

HttpRequest makeGet(const std::string& clientId, const std::string& accessToken, const std::string& path,
                    const std::vector<std::pair<std::string, std::string>>& params) {
  HttpRequest req;
  req.method = "GET";
  req.url = std::string(kHelixUrl) + path;
  if (!params.empty()) req.url += "?" + buildQuery(params);
  req.headers = {{"Authorization", "Bearer " + accessToken}, {"Client-Id", clientId}};
  return req;
}

// GET /users takes up to 100 id parameters in one request.
constexpr std::size_t kUsersPerRequest = 100;

// Twitch caps `first` per page; 100 is the documented max for every Helix
// list endpoint used here.
int clampFirst(int want) {
  if (want <= 0) return 0;
  return std::min(want, 100);
}

// Helix thumbnail_url is a template with literal "{width}" / "{height}"
// placeholders, e.g. ".../live_user_x-{width}x{height}.jpg". Substituting
// 440x248 here matches the size browse.cpp asks GraphQL for
// (previewImageURL(width: 440, height: 248)), so the same liveThumbnailUrl
// bucketing behaves the same way for both sources.
std::string thumbnailSized(std::string url, int width, int height) {
  auto replaceAll = [&](const std::string& token, const std::string& value) {
    size_t pos = 0;
    while ((pos = url.find(token, pos)) != std::string::npos) {
      url.replace(pos, token.size(), value);
      pos += value.size();
    }
  };
  replaceAll("{width}", std::to_string(width));
  replaceAll("{height}", std::to_string(height));
  return url;
}

// data[] from a Helix list response. Throws ApiError when `data` itself is
// missing or not an array; a malformed individual element is simply skipped
// by the caller (objectOr-style leniency, same as browse.cpp's parseEdges).
const nlohmann::json& dataArrayOf(const nlohmann::json& body, const char* what) {
  auto it = body.find("data");
  if (it == body.end() || !it->is_array()) throw ApiError(std::string(what) + ".data missing");
  return *it;
}

std::string cursorOf(const nlohmann::json& body) {
  const nlohmann::json* pagination = objectOr(&body, "pagination");
  return pagination ? stringOr(pagination, "cursor") : std::string();
}

}  // namespace

HelixClient::HelixClient(HttpClient& http, std::string clientId, std::string accessToken)
    : http_(http), clientId_(std::move(clientId)), accessToken_(std::move(accessToken)) {}

HelixUser HelixClient::me() {
  HttpRequest req = makeGet(clientId_, accessToken_, "/users", {});
  HttpResponse res = http_.send(req);
  checkStatus(res, "users");

  nlohmann::json body = parseBody(res);
  if (!body.is_object()) throw ApiError("users response is not an object");
  const nlohmann::json& data = dataArrayOf(body, "users");
  if (data.empty() || !data[0].is_object()) throw ApiError("users response is missing data[0]");

  HelixUser out;
  out.id = stringOr(&data[0], "id");
  out.login = stringOr(&data[0], "login");
  out.displayName = stringOr(&data[0], "display_name");
  out.profileImageUrl = stringOr(&data[0], "profile_image_url");
  if (out.id.empty() || out.login.empty()) throw ApiError("users response is missing id/login");
  return out;
}

std::vector<HelixUser> HelixClient::usersById(const std::vector<std::string>& ids) {
  std::vector<HelixUser> out;
  std::vector<std::pair<std::string, std::string>> params;
  auto flush = [&] {
    if (params.empty()) return;
    HttpResponse res = http_.send(makeGet(clientId_, accessToken_, "/users", params));
    params.clear();
    checkStatus(res, "users");
    nlohmann::json body = parseBody(res);
    if (!body.is_object()) throw ApiError("users response is not an object");
    for (const auto& item : dataArrayOf(body, "users")) {
      if (!item.is_object()) continue;
      HelixUser u;
      u.id = stringOr(&item, "id");
      u.login = stringOr(&item, "login");
      u.displayName = stringOr(&item, "display_name");
      u.profileImageUrl = stringOr(&item, "profile_image_url");
      if (u.id.empty() || u.login.empty()) continue;
      out.push_back(std::move(u));
    }
  };
  for (const std::string& id : ids) {
    if (id.empty()) continue;
    params.push_back({"id", id});
    if (params.size() == kUsersPerRequest) flush();
  }
  flush();
  return out;
}

std::string HelixClient::userIdForLogin(const std::string& login) {
  const std::string normalized = normalizeLogin(login);  // throws NotFoundError, no request
  HttpRequest req = makeGet(clientId_, accessToken_, "/users", {{"login", normalized}});
  HttpResponse res = http_.send(req);
  checkStatus(res, "users");

  nlohmann::json body = parseBody(res);
  if (!body.is_object()) throw ApiError("users response is not an object");
  const nlohmann::json& data = dataArrayOf(body, "users");
  if (data.empty()) throw NotFoundError("no such user: " + normalized);
  if (!data[0].is_object()) throw ApiError("users response data[0] has the wrong shape");

  std::string id = stringOr(&data[0], "id");
  if (id.empty()) throw ApiError("users response is missing id");
  return id;
}

std::vector<FollowedStream> HelixClient::followedStreams(const std::string& userId, long long nowSeconds,
                                                          int max) {
  std::vector<FollowedStream> out;
  if (max <= 0) return out;

  std::string after;
  bool hasAfter = false;
  while (static_cast<int>(out.size()) < max) {
    const int want = clampFirst(max - static_cast<int>(out.size()));
    std::vector<std::pair<std::string, std::string>> params = {{"user_id", userId},
                                                                 {"first", std::to_string(want)}};
    if (hasAfter) params.push_back({"after", after});

    HttpResponse res = http_.send(makeGet(clientId_, accessToken_, "/streams/followed", params));
    checkStatus(res, "followed streams");

    nlohmann::json body = parseBody(res);
    if (!body.is_object()) throw ApiError("followed streams response is not an object");
    const nlohmann::json& data = dataArrayOf(body, "followed streams");

    for (const auto& item : data) {
      if (!item.is_object()) continue;
      FollowedStream s;
      s.userId = stringOr(&item, "user_id");
      s.login = stringOr(&item, "user_login");
      s.displayName = stringOr(&item, "user_name");
      s.title = stringOr(&item, "title");
      s.game = stringOr(&item, "game_name");
      s.viewers = intOr(&item, "viewer_count");
      s.thumbUrl = liveThumbnailUrl(thumbnailSized(stringOr(&item, "thumbnail_url"), 440, 248), nowSeconds);
      out.push_back(std::move(s));
      if (static_cast<int>(out.size()) >= max) break;
    }

    const std::string cursor = cursorOf(body);
    if (cursor.empty() || data.empty()) break;  // no more pages, or Twitch gave us nothing to page from
    after = cursor;
    hasAfter = true;
  }
  return out;
}

std::vector<FollowedChannel> HelixClient::followedChannels(const std::string& userId, int max) {
  std::vector<FollowedChannel> out;
  if (max <= 0) return out;

  std::string after;
  bool hasAfter = false;
  while (static_cast<int>(out.size()) < max) {
    const int want = clampFirst(max - static_cast<int>(out.size()));
    std::vector<std::pair<std::string, std::string>> params = {{"user_id", userId},
                                                                 {"first", std::to_string(want)}};
    if (hasAfter) params.push_back({"after", after});

    HttpResponse res = http_.send(makeGet(clientId_, accessToken_, "/channels/followed", params));
    checkStatus(res, "followed channels");

    nlohmann::json body = parseBody(res);
    if (!body.is_object()) throw ApiError("followed channels response is not an object");
    const nlohmann::json& data = dataArrayOf(body, "followed channels");

    for (const auto& item : data) {
      if (!item.is_object()) continue;
      FollowedChannel c;
      c.userId = stringOr(&item, "broadcaster_id");
      c.login = stringOr(&item, "broadcaster_login");
      c.displayName = stringOr(&item, "broadcaster_name");
      out.push_back(std::move(c));
      if (static_cast<int>(out.size()) >= max) break;
    }

    const std::string cursor = cursorOf(body);
    if (cursor.empty() || data.empty()) break;
    after = cursor;
    hasAfter = true;
  }
  return out;
}

SendResult HelixClient::sendChat(const std::string& broadcasterId, const std::string& senderId,
                                 const std::string& message) {
  HttpRequest req;
  req.method = "POST";
  req.url = std::string(kHelixUrl) + "/chat/messages";
  req.headers = {{"Authorization", "Bearer " + accessToken_},
                 {"Client-Id", clientId_},
                 {"Content-Type", "application/json"}};
  // error_handler_t::replace: invalid UTF-8 in console-typed text can never
  // throw here (same defensive choice as gql.cpp's query body).
  req.body = nlohmann::json{{"broadcaster_id", broadcasterId},
                            {"sender_id", senderId},
                            {"message", message}}
                 .dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);

  HttpResponse res = http_.send(req);
  checkStatus(res, "send chat");

  nlohmann::json body = parseBody(res);
  if (!body.is_object()) throw ApiError("send chat response is not an object");
  const nlohmann::json& data = dataArrayOf(body, "send chat");
  if (data.empty() || !data[0].is_object()) throw ApiError("send chat response is missing data[0]");

  SendResult out;
  out.sent = boolOr(&data[0], "is_sent", false);
  if (!out.sent) {
    const nlohmann::json* dropReason = objectOr(&data[0], "drop_reason");
    out.dropReason = dropReason ? stringOr(dropReason, "message") : std::string();
  }
  return out;
}

CreatedClip HelixClient::createClip(const std::string& broadcasterId) {
  HttpRequest req;
  req.method = "POST";
  req.url = std::string(kHelixUrl) + "/clips?" + buildQuery({{"broadcaster_id", broadcasterId}});
  req.headers = {{"Authorization", "Bearer " + accessToken_}, {"Client-Id", clientId_}};

  HttpResponse res = http_.send(req);
  checkStatus(res, "create clip");

  nlohmann::json body = parseBody(res);
  if (!body.is_object()) throw ApiError("create clip response is not an object");
  const nlohmann::json& data = dataArrayOf(body, "create clip");
  if (data.empty() || !data[0].is_object()) throw ApiError("create clip response is missing data[0]");

  CreatedClip out;
  out.id = stringOr(&data[0], "id");
  out.editUrl = stringOr(&data[0], "edit_url");
  if (out.id.empty()) throw ApiError("create clip response is missing id");
  // The id goes into the clip's address (and a QR of it) as it is: a slug
  // is letters, digits, '-' and '_'.
  for (const char c : out.id) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                    c == '_';
    if (!ok) throw ApiError("create clip response has an id that isn't a slug");
  }
  return out;
}

}  // namespace twitch
