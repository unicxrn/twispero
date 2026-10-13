#pragma once
// evo_twitch_clip.h - making a clip of the live stream playing (Square in the
// player), as the logged-in user.
//
// A worker of its own, started by the first clip, makes one clip at a time:
// the user's token (auth_token(), which may wait for a refresh), the
// channel's user id (asked for once per channel, then kept), then Helix's
// POST /clips over verified TLS. A 401 for the token is reported and the
// clip asked for once more after the refresh, as chat does; a request that
// may have reached Twitch is never sent again. Tokens never reach the log.
// The outcome comes back once, through clip_take_result().
//
// Main thread only.

#include <string>

namespace evo_twitch {

// A clip's page is this followed by its id; kClipPageShort is the same
// address as a toast writes it.
inline constexpr const char* kClipPageUrl = "https://clips.twitch.tv/";
inline constexpr const char* kClipPageShort = "clips.twitch.tv/";

// How long the player shows the card with the clip's QR (Circle closes it
// sooner).
inline constexpr unsigned long long kClipCardMs = 20000;

struct ClipResult {
  bool made = false;
  std::string id;       // made: the clip's id; its page is kClipPageUrl + id
  std::string channel;  // the login it was made of, lower case
  std::string error;    // not made: the toast that says why
};

// Asks for a clip of `channel`'s live stream (a login, any case). nullptr when
// it is on its way (the player toasts "Making a clip..."); otherwise, at once,
// the toast that says why not:
//   "Checking your Twitch login"            the saved login isn't read yet
//   "Clips need a login"                    logged out
//   "Log in again to make clips"            the login is older than clips
//                                           (no clips:edit: auth_has_scope())
//   "Already making a clip"                 one is on its way, or its outcome
//                                           hasn't been taken yet
//   "Clips aren't available in this build"  no worker (the self-test failed)
const char* clip_make(const std::string& channel) noexcept;

// The last clip's outcome, once: moved to *out, and true. What a failure says:
//   "This channel doesn't allow clips"  Twitch's 403: clips are off there, or
//                                       only for followers or subscribers
//   "This channel is offline"           Twitch's 404: the stream has ended
//   "Log in again to make clips"        Twitch refused the token twice, or
//                                       says clips:edit is missing
//   "Clips need a login"                logged out (or another user logged
//                                       in) before it went out
//   "Try again in a moment"             429, or Twitch had a problem (5xx)
//   "No connection", "Check the PS5's date and time",
//   "This channel wasn't found", "Twitch didn't answer as expected",
//   "Twitch couldn't make the clip" (any other status), "Something went wrong"
//   "Clips aren't available in this build" (the worker can't run the core)
bool clip_take_result(ClipResult* out) noexcept;

}  // namespace evo_twitch
