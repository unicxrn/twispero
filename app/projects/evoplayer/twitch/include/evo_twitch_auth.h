#pragma once
// evo_twitch_auth.h - logging in to Twitch, and the user's token for Helix.
//
// One worker thread owns the login. It is the only code that ever holds the
// refresh token, which Twitch accepts once only. It:
//   - reads <data root>/auth.json once the sandbox is open (until then the
//     state is Unknown), and writes it after every login and refresh, before
//     the new token is handed out: a .tmp file, fsync, mode 0600, a rename,
//     then the folder fsync'd. A failed write is retried until it works. An
//     auth.json that can't be read is moved to auth.json.bad;
//   - runs the device-code login: a code and a QR on screen, which the user
//     approves on their phone at twitch.tv/activate;
//   - validates the token at start and then every hour, as Twitch asks. A
//     validation that can't reach Twitch keeps the saved login (offline is
//     not a logout); only a refused token leads to a refresh;
//   - refreshes when less than 5 minutes are left, or after a reported 401.
//     When Twitch refuses the refresh, the user is logged out and the view
//     carries the notice "Your Twitch login ran out. Log in again." once.
// Every request goes over verified TLS (evoTwitchHttpVerified()). Tokens,
// refresh tokens and codes never reach the log.
//
// Threads: auth_token() and auth_token_rejected() work from any thread;
// everything else is main thread only.

#include <string>
#include <vector>

#include "twitch/auth.hpp"
#include "twitch/qr.hpp"

namespace evo_twitch {

enum class AuthState { Unknown, LoggedOut, Starting, WaitingForUser, LoggedIn, Error };

struct AuthView {
  AuthState state = AuthState::Unknown;
  std::string login, displayName, userId;  // when LoggedIn
  // When LoggedIn: the scopes Twitch granted, as it last said (the login,
  // a refresh or the hourly validation); empty when it never listed them.
  // auth_has_scope() reads them.
  std::vector<std::string> scopes;
  std::string userCode, verificationUri;   // when WaitingForUser
  twitch::QrMatrix qr;                     // when WaitingForUser: verificationUri as Twitch sent it
                                           // (it already carries the code)
  long long codeExpiresAtMs = 0;           // when WaitingForUser, on data_now_ms()'s clock
  std::string error;                       // a plain sentence when Error
};

// Spawns the worker once and looks for auth.json; later calls do nothing.
// Call after chat_start(). If the self-test failed or the worker can't run
// the core, the state is Error ("Logging in isn't available in this build").
void auth_start() noexcept;

// Once per frame, next to data_frame(): hands the worker the file's path once
// the sandbox opens, and moves what the worker did into the view.
void auth_frame() noexcept;

// The same object for the life of the app. Its contents change only inside
// auth_start(), auth_frame() and the calls below.
const AuthView& auth_view() noexcept;

// Starts the device-code login: Starting at once, then WaitingForUser with
// the code and the QR, then LoggedIn (or Error with the reason). Does nothing
// while LoggedIn, while a login is already on its way, or while the saved
// login hasn't been read yet (Unknown). From Error it starts over.
void auth_begin_login() noexcept;

// Drops a login on its way, or the Error one ended with: LoggedOut at once.
void auth_cancel_login() noexcept;

// LoggedOut at once, and auth_token() says false from now on. The worker then
// deletes auth.json and only after that revokes both tokens (best effort).
void auth_logout() noexcept;

// The one-off message for a toast, if there is one ("Your Twitch login ran
// out. Log in again."): moves it to *out and returns true. Each notice is
// handed out once; auth_begin_login() and auth_logout() drop one not yet taken.
bool auth_take_notice(std::string* out) noexcept;

// The login has `scope` (twitch::kClipScope, say): logins from before the
// app asked for it don't, and only a new login brings it. A login whose
// scopes Twitch never listed counts as having it, and Helix's answer decides.
// False unless LoggedIn. Main thread, with auth_view().
inline bool auth_has_scope(const AuthView& view, const char* scope) noexcept {
  if (view.state != AuthState::LoggedIn) return false;
  return view.scopes.empty() || twitch::hasScope(view.scopes, scope);
}

// Any thread but the main one: a valid access token and the user id for a
// Helix call, or false when logged out. When the token has less than 5
// minutes left, or a refresh is on its way, it waits for the worker's refresh,
// up to 10 s. After a refresh that got no answer, callers get the old token
// at once until the worker tries again (5 s later, doubling up to 5 min);
// while that retry is out they wait for it like any refresh, and a network
// that times out can use up the whole 10 s.
// On the main thread it never waits: it returns what's there (asking for the
// refresh if one is due) and logs an error line, once.
bool auth_token(std::string* accessToken, std::string* userId) noexcept;

// Any thread: Helix answered 401 to accessToken. The worker refreshes once,
// however many callers report the same token; a token already replaced is
// ignored. When Twitch refuses the refresh, the user is logged out.
void auth_token_rejected(const std::string& accessToken) noexcept;

}  // namespace evo_twitch
