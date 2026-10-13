#pragma once
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "twitch/http.hpp"

namespace twitch {

inline constexpr const char* kIdTwitchUrl = "https://id.twitch.tv";

// "k1=v1&k2=v2", percent-encoding both keys and values (RFC 3986 unreserved
// characters kept, same escaping as url.hpp's percentEncode). Suitable as an
// application/x-www-form-urlencoded body - id.twitch.tv's /oauth2/token only
// reads a form body, never a query string or JSON.
std::string formEncode(const std::vector<std::pair<std::string, std::string>>& fields);

// Answer from starting a device-code login: show userCode/verificationUri
// (or a QR of verificationUri, see qr.hpp) to the user, then poll.
struct DeviceCode {
  std::string deviceCode;
  std::string userCode;
  std::string verificationUri;
  int intervalSec = 5;
  int expiresInSec = 1800;
};

// A user's access/refresh token pair, plus when the access token expires.
struct Tokens {
  std::string access;
  std::string refresh;
  long long expiresAtMs = 0;
  std::vector<std::string> scopes;
};

// True when `scopes` (as Twitch granted them: Tokens::scopes,
// Identity::scopes) holds `scope` exactly. Never throws.
bool hasScope(const std::vector<std::string>& scopes, const std::string& scope) noexcept;

// What GET /oauth2/validate says about an access token.
struct Identity {
  std::string userId;
  std::string login;
  int expiresInSec = 0;
  std::vector<std::string> scopes;
};

enum class PollResult { Pending, SlowDown, Done, Expired, Denied };

// The Twitch OAuth device-code flow against id.twitch.tv, for a Public
// client (no client secret). Pass a verified HttpClient
// (evoTwitchHttpVerified() in the app); any HttpClient while testing with
// FakeHttpClient.
class AuthClient {
 public:
  AuthClient(HttpClient& http, std::string clientId);

  // POST /oauth2/device (form body: client_id, scopes).
  // Throws ApiError on a non-200 status or an unexpected/incomplete body.
  DeviceCode startDeviceLogin(const std::string& scopes);

  // POST /oauth2/token (form body: client_id, scopes, device_code,
  // grant_type=urn:ietf:params:oauth:grant-type:device_code). Twitch answers
  // a still-pending login with 400, which this turns into a PollResult
  // instead of throwing; only an unexpected shape (not one of the known 400
  // messages, and not 200) throws ApiError. On PollResult::Done, *out is
  // filled, with expiresAtMs = nowMs + expires_in*1000; `out` may be null.
  PollResult pollDeviceLogin(const DeviceCode& code, const std::string& scopes, long long nowMs,
                              Tokens* out);

  // POST /oauth2/token (form body: grant_type=refresh_token, refresh_token,
  // client_id). A refresh token works once; the answer carries a new one.
  // Throws UnauthorizedError when Twitch refuses it (400 or 401).
  Tokens refresh(const std::string& refreshToken, long long nowMs);

  // GET /oauth2/validate with "Authorization: OAuth <token>".
  // Throws UnauthorizedError on 401.
  Identity validate(const std::string& accessToken);

  // POST /oauth2/revoke (form body: client_id, token). Best effort: never
  // throws, so logout can call it without a try/catch.
  void revoke(const std::string& accessToken) noexcept;

 private:
  HttpClient& http_;
  std::string clientId_;
};

// auth.json's contents: everything the app needs to resume a session
// without asking the user to log in again.
struct SavedLogin {
  Tokens tokens;
  std::string userId;
  std::string login;
  std::string displayName;
};

// {"access":..,"refresh":..,"expires_at_ms":..,"scopes":[..],"user_id":..,
//  "login":..,"display_name":..}
std::string serializeLogin(const SavedLogin& login);

// Never throws: nullopt on bad JSON, a non-object, or a missing/wrong-typed
// required field (access, refresh, expires_at_ms, user_id, login).
std::optional<SavedLogin> parseLogin(const std::string& json) noexcept;

}  // namespace twitch
