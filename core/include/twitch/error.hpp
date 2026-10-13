#pragma once
#include <stdexcept>
#include <string>

namespace twitch {

// Base class for every error the core throws.
class TwitchError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// Transport failed (DNS, TLS, timeout, connection closed).
class NetworkError : public TwitchError {
 public:
  using TwitchError::TwitchError;
};

// Twitch answered, but not in the shape we expect. Usually means Twitch changed something.
class ApiError : public TwitchError {
 public:
  using TwitchError::TwitchError;
};

// Thrown when a Twitch endpoint answers with an unexpected HTTP status;
// 5xx/429 are usually temporary.
class HttpStatusError : public ApiError {
 public:
  HttpStatusError(const std::string& what, int status) : ApiError(what), status_(status) {}
  int status() const { return status_; }

 private:
  int status_;
};

// The channel (or video) does not exist.
class NotFoundError : public TwitchError {
 public:
  using TwitchError::TwitchError;
};

// Twitch refused a token: 401 from an endpoint that answers that way
// (validate, Helix), or the equivalent refusal from one that doesn't
// (refresh and the device-code poll answer a refused/expired/revoked
// refresh token with 400). Callers that see this should drop the saved
// login and send the user back to the login screen.
//
// Helix also answers 401 when the token is fine but lacks a scope the call
// needs ("Missing scope: user:read:follows"). That one has missingScope()
// true: a refresh can't help (it keeps the scopes), only a new login can, so
// callers must not report the token as refused for it.
class UnauthorizedError : public ApiError {
 public:
  using ApiError::ApiError;
  UnauthorizedError(const std::string& what, bool missingScope) : ApiError(what), missingScope_(missingScope) {}
  bool missingScope() const { return missingScope_; }

 private:
  bool missingScope_ = false;
};

// The channel exists but is not live.
class OfflineError : public TwitchError {
 public:
  using TwitchError::TwitchError;
};

// The video or clip exists, but anonymous playback is refused: a subscriber-only VOD, caught
// either from the playback token's own authorization.forbidden flag or from usher answering
// 403 anyway. A login would not help - the core never authenticates playback with a viewer's
// own token, so there is nothing a retry can do differently.
class SubscriberOnlyError : public TwitchError {
 public:
  using TwitchError::TwitchError;
};

}  // namespace twitch
