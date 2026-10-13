#include "twitch/auth.hpp"

#include <nlohmann/json.hpp>

#include "json_helpers.hpp"
#include "twitch/error.hpp"
#include "twitch/url.hpp"

namespace twitch {
namespace {

// Pulls every string out of a JSON array field; missing, non-array or
// non-string entries are skipped rather than thrown on.
std::vector<std::string> stringArrayOr(const nlohmann::json& obj, const char* key) {
  std::vector<std::string> out;
  if (!obj.is_object()) return out;
  auto it = obj.find(key);
  if (it == obj.end() || !it->is_array()) return out;
  for (const auto& item : *it)
    if (item.is_string()) out.push_back(item.get<std::string>());
  return out;
}

nlohmann::json parseBody(const HttpResponse& res) {
  return nlohmann::json::parse(res.body, nullptr, /*allow_exceptions=*/false);
}

}  // namespace

bool hasScope(const std::vector<std::string>& scopes, const std::string& scope) noexcept {
  for (const std::string& s : scopes)
    if (s == scope) return true;
  return false;
}

std::string formEncode(const std::vector<std::pair<std::string, std::string>>& fields) {
  return buildQuery(fields);
}

AuthClient::AuthClient(HttpClient& http, std::string clientId)
    : http_(http), clientId_(std::move(clientId)) {}

DeviceCode AuthClient::startDeviceLogin(const std::string& scopes) {
  HttpRequest req;
  req.method = "POST";
  req.url = std::string(kIdTwitchUrl) + "/oauth2/device";
  req.headers = {{"Content-Type", "application/x-www-form-urlencoded"}};
  req.body = formEncode({{"client_id", clientId_}, {"scopes", scopes}});

  HttpResponse res = http_.send(req);
  if (res.status != 200) throw HttpStatusError("device HTTP " + std::to_string(res.status), res.status);

  nlohmann::json body = parseBody(res);
  if (!body.is_object()) throw ApiError("device response is not an object");

  DeviceCode out;
  out.deviceCode = stringOr(&body, "device_code");
  out.userCode = stringOr(&body, "user_code");
  out.verificationUri = stringOr(&body, "verification_uri");
  out.intervalSec = intOrDefault(&body, "interval", 5);
  out.expiresInSec = intOrDefault(&body, "expires_in", 1800);
  if (out.deviceCode.empty() || out.userCode.empty() || out.verificationUri.empty())
    throw ApiError("device response is missing a required field");
  return out;
}

PollResult AuthClient::pollDeviceLogin(const DeviceCode& code, const std::string& scopes, long long nowMs,
                                         Tokens* out) {
  HttpRequest req;
  req.method = "POST";
  req.url = std::string(kIdTwitchUrl) + "/oauth2/token";
  req.headers = {{"Content-Type", "application/x-www-form-urlencoded"}};
  req.body = formEncode({
      {"client_id", clientId_},
      {"scopes", scopes},
      {"device_code", code.deviceCode},
      {"grant_type", "urn:ietf:params:oauth:grant-type:device_code"},
  });

  HttpResponse res = http_.send(req);

  if (res.status == 400) {
    nlohmann::json body = parseBody(res);
    std::string message = body.is_object() ? stringOr(&body, "message") : "";
    if (message == "authorization_pending") return PollResult::Pending;
    if (message == "slow_down") return PollResult::SlowDown;
    if (message == "expired_token" || message == "invalid device code") return PollResult::Expired;
    if (message == "access_denied") return PollResult::Denied;
    throw ApiError("device poll HTTP 400: " + (message.empty() ? "unknown error" : message));
  }

  if (res.status != 200) throw HttpStatusError("device poll HTTP " + std::to_string(res.status), res.status);

  nlohmann::json body = parseBody(res);
  if (!body.is_object()) throw ApiError("device poll response is not an object");

  std::string access = stringOr(&body, "access_token");
  if (access.empty()) throw ApiError("device poll response is missing access_token");

  if (out != nullptr) {
    out->access = access;
    out->refresh = stringOr(&body, "refresh_token");
    out->expiresAtMs = nowMs + static_cast<long long>(intOrDefault(&body, "expires_in", 0)) * 1000;
    out->scopes = stringArrayOr(body, "scope");
  }
  return PollResult::Done;
}

Tokens AuthClient::refresh(const std::string& refreshToken, long long nowMs) {
  HttpRequest req;
  req.method = "POST";
  req.url = std::string(kIdTwitchUrl) + "/oauth2/token";
  req.headers = {{"Content-Type", "application/x-www-form-urlencoded"}};
  req.body = formEncode({
      {"grant_type", "refresh_token"},
      {"refresh_token", refreshToken},
      {"client_id", clientId_},
  });

  HttpResponse res = http_.send(req);
  if (res.status == 400 || res.status == 401)
    throw UnauthorizedError("refresh HTTP " + std::to_string(res.status));
  if (res.status != 200) throw HttpStatusError("refresh HTTP " + std::to_string(res.status), res.status);

  nlohmann::json body = parseBody(res);
  if (!body.is_object()) throw ApiError("refresh response is not an object");

  Tokens out;
  out.access = stringOr(&body, "access_token");
  out.refresh = stringOr(&body, "refresh_token");
  // Twitch always rotates the refresh token on a successful refresh, but if
  // a 200 answer ever lacks one, keep the caller's old refresh token rather
  // than throwing: it's only burned once Twitch actually sends a
  // replacement, so this is purely defensive against an answer that's
  // otherwise usable.
  if (out.refresh.empty()) out.refresh = refreshToken;
  out.expiresAtMs = nowMs + static_cast<long long>(intOrDefault(&body, "expires_in", 0)) * 1000;
  out.scopes = stringArrayOr(body, "scope");
  if (out.access.empty()) throw ApiError("refresh response is missing access_token");
  return out;
}

Identity AuthClient::validate(const std::string& accessToken) {
  HttpRequest req;
  req.method = "GET";
  req.url = std::string(kIdTwitchUrl) + "/oauth2/validate";
  req.headers = {{"Authorization", "OAuth " + accessToken}};

  HttpResponse res = http_.send(req);
  if (res.status == 401) throw UnauthorizedError("validate HTTP 401");
  if (res.status != 200) throw HttpStatusError("validate HTTP " + std::to_string(res.status), res.status);

  nlohmann::json body = parseBody(res);
  if (!body.is_object()) throw ApiError("validate response is not an object");

  Identity out;
  out.userId = stringOr(&body, "user_id");
  out.login = stringOr(&body, "login");
  out.expiresInSec = intOrDefault(&body, "expires_in", 0);
  out.scopes = stringArrayOr(body, "scopes");
  return out;
}

void AuthClient::revoke(const std::string& accessToken) noexcept {
  try {
    HttpRequest req;
    req.method = "POST";
    req.url = std::string(kIdTwitchUrl) + "/oauth2/revoke";
    req.headers = {{"Content-Type", "application/x-www-form-urlencoded"}};
    req.body = formEncode({{"client_id", clientId_}, {"token", accessToken}});
    http_.send(req);
  } catch (...) {
    // Best effort: a dead network shouldn't block logging out locally.
  }
}

std::string serializeLogin(const SavedLogin& login) {
  nlohmann::json j;
  j["access"] = login.tokens.access;
  j["refresh"] = login.tokens.refresh;
  j["expires_at_ms"] = login.tokens.expiresAtMs;
  j["scopes"] = login.tokens.scopes;
  j["user_id"] = login.userId;
  j["login"] = login.login;
  j["display_name"] = login.displayName;
  return j.dump() + "\n";
}

std::optional<SavedLogin> parseLogin(const std::string& json) noexcept {
  try {
    const nlohmann::json j = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return std::nullopt;

    auto requiredString = [&](const char* key) -> std::optional<std::string> {
      auto it = j.find(key);
      if (it == j.end() || !it->is_string() || it->get<std::string>().empty()) return std::nullopt;
      return it->get<std::string>();
    };

    auto access = requiredString("access");
    auto refreshTok = requiredString("refresh");
    auto userId = requiredString("user_id");
    auto loginName = requiredString("login");
    auto expiresAtMs = j.find("expires_at_ms");
    if (!access || !refreshTok || !userId || !loginName) return std::nullopt;
    if (expiresAtMs == j.end() || !expiresAtMs->is_number_integer()) return std::nullopt;

    SavedLogin out;
    out.tokens.access = *access;
    out.tokens.refresh = *refreshTok;
    out.tokens.expiresAtMs = expiresAtMs->get<long long>();
    out.tokens.scopes = stringArrayOr(j, "scopes");
    out.userId = *userId;
    out.login = *loginName;
    if (auto dn = j.find("display_name"); dn != j.end() && dn->is_string())
      out.displayName = dn->get<std::string>();
    return out;
  } catch (...) {
    return std::nullopt;
  }
}

}  // namespace twitch
