#pragma once
#include <string>
#include <vector>

#include "twitch/http.hpp"

namespace twitch {

inline constexpr const char* kHelixUrl = "https://api.twitch.tv/helix";

// Who a user access token belongs to. auth.hpp's Identity is what
// /oauth2/validate answers, which has no display name; Helix's own /users
// does, so HelixClient::me() returns this instead of Identity.
struct HelixUser {
  std::string id;
  std::string login;
  std::string displayName;
  std::string profileImageUrl;  // 300x300 as Twitch hands it out; "" when it gave none
};

// One followed channel that's currently live. thumbUrl is already run
// through liveThumbnailUrl() at 440x248 (same size and 5-minute bucket as
// the existing stream cards - browse.cpp/streams.cpp - so the image cache
// behaves the same way).
struct FollowedStream {
  std::string userId, login, displayName, title, game, thumbUrl;
  int viewers = 0;
};

// One followed channel, live or not.
struct FollowedChannel {
  std::string userId, login, displayName;
};

// What POST /chat/messages answered. `sent` false means Twitch accepted the
// request but didn't deliver it (rate limit, banned term, etc.);
// `dropReason` is Twitch's own message for the user and is empty when
// `sent` is true.
struct SendResult {
  bool sent = false;
  std::string dropReason;
};

// What POST /clips answered: the new clip's id, which is also its slug (the
// clip's page is https://clips.twitch.tv/<id>), and the page where its maker
// can trim and title it. Twitch answers at once and makes the clip over the
// next few seconds, so the clip may not play yet.
struct CreatedClip {
  std::string id;
  std::string editUrl;
};

// Twitch's Helix API (api.twitch.tv/helix) with a user access token. Pass a
// verified HttpClient (evoTwitchHttpVerified() in the app); any HttpClient
// while testing with FakeHttpClient.
//
// Every call sends "Authorization: Bearer <token>" and "Client-Id: <id>".
// A 401 throws UnauthorizedError (with missingScope() true when Twitch says
// the token lacks a scope the call needs); any other non-2xx status
// (including a 3xx) throws HttpStatusError. Never put the token in an
// exception message.
class HelixClient {
 public:
  HelixClient(HttpClient& http, std::string clientId, std::string accessToken);

  // GET /users (no params): the token's own user.
  HelixUser me();

  // GET /users?id=<id>&id=<id>...: those users, profile pictures included,
  // 100 ids to a request. Empty ids are skipped; no request at all for none.
  // Users Twitch doesn't return (banned, deleted) are simply missing, and the
  // order is Twitch's, so match the answer by id.
  std::vector<HelixUser> usersById(const std::vector<std::string>& ids);

  // GET /users?login=<login>: that login's user id.
  // Throws NotFoundError when Twitch returns no matching user (also for an
  // invalid login, via normalizeLogin, with no request sent).
  std::string userIdForLogin(const std::string& login);

  // GET /streams/followed?user_id=<userId>&first=<n>(&after=<cursor>),
  // paging with the returned cursor until `max` streams are collected or a
  // page comes back without a cursor (or without any streams). Needs scope
  // user:read:follows. `nowSeconds` is the wall-clock time used for each
  // thumbUrl's cache-busting bucket (see liveThumbnailUrl in browse.hpp);
  // callers pass it explicitly, same as AuthClient's nowMs, so the bucket is
  // deterministic and testable rather than read from the clock in here.
  std::vector<FollowedStream> followedStreams(const std::string& userId, long long nowSeconds,
                                               int max = 100);

  // GET /channels/followed?user_id=<userId>&first=<n>(&after=<cursor>),
  // same pagination as followedStreams. Needs scope user:read:follows.
  std::vector<FollowedChannel> followedChannels(const std::string& userId, int max = 300);

  // POST /chat/messages, JSON body {broadcaster_id, sender_id, message}.
  // Needs scope user:write:chat. The message is sent through nlohmann's own
  // JSON string escaping (quotes, control characters).
  SendResult sendChat(const std::string& broadcasterId, const std::string& senderId,
                      const std::string& message);

  // POST /clips?broadcaster_id=<id>, no body: a clip of that channel's live
  // stream, made by the token's user. Needs scope clips:edit. Twitch answers
  // 202 Accepted with data[0] {id, edit_url}. 403 means the channel doesn't
  // allow clips (clips off, or only for followers or subscribers) and 404
  // that it isn't live: both an HttpStatusError, like any other status (a
  // 401 throws UnauthorizedError as everywhere here). Throws ApiError when
  // the answer has no id.
  CreatedClip createClip(const std::string& broadcasterId);

 private:
  HttpClient& http_;
  std::string clientId_;
  std::string accessToken_;
};

}  // namespace twitch
