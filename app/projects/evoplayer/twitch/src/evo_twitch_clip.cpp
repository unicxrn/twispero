// Making a clip as the logged-in user. The contract is in evo_twitch_clip.h.
//
//   main thread                                    clip worker (1, started by the first clip)
//   clip_make(channel)
//     not logged in, no clips:edit, one already
//     on its way      -> the toast, at once
//     otherwise       -> Job --slot (g_mx)--------> auth_token() (may wait for a refresh),
//                                                   the channel's user id (kept),
//                                                   POST /clips?broadcaster_id=;
//                                                   a 401 for the token: reported, and
//                                                   asked once more after the refresh
//   clip_take_result() <--result (g_mx)----------- made (the clip's id) or why not
//
// One clip at a time: a second Square while the first is out, or before its
// outcome is taken, is turned down ("Already making a clip"). Twitch makes a
// clip of the last seconds of the stream, so a queue of them would only make
// copies.
//
// Why a thread of its own and not the chat sender's: that one keeps a queue
// of messages and their drafts, and a clip can wait 10 s for a token refresh
// on top of Twitch's answer, which would hold the messages up behind it. The
// thread sleeps on its condvar between clips, and a user who never makes one
// never starts it.
//
// File-scope state is trivially constructible, like evo_twitch_chat_send.cpp:
// the C++ objects are placement-constructed into static storage on first use
// and never destroyed.

#include "evo_twitch_clip.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include <atomic>
#include <cstddef>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "evo_boot_log.h"
#include "evo_twitch.h"
#include "evo_twitch_auth.h"
#include "evo_twitch_data.h"
#include "evo_twitch_internal.h"
#include "twitch/app.hpp"
#include "twitch/error.hpp"
#include "twitch/helix.hpp"

namespace evo_twitch {
namespace {

constexpr size_t kStackBytes = 2 * 1024 * 1024;  // TLS with certificate checks, as the other workers
constexpr size_t kMaxIds = 32;                   // channels whose user id is remembered

// clip_make()'s toasts.
const char* const kRefusedChecking = "Checking your Twitch login";
const char* const kRefusedLoggedOut = "Clips need a login";
const char* const kRefusedScope = "Log in again to make clips";
const char* const kRefusedBusy = "Already making a clip";
const char* const kRefusedNoWorker = "Clips aren't available in this build";

// What a clip that wasn't made says.
const char* const kWhyLoggedOut = kRefusedLoggedOut;
const char* const kWhyLogInAgain = kRefusedScope;
const char* const kWhyNotAllowed = "This channel doesn't allow clips";
const char* const kWhyTryAgain = "Try again in a moment";
const char* const kWhyRefused = "Twitch couldn't make the clip";
const char* const kWhyClock = "Check the PS5's date and time";
const char* const kWhyNotFound = "This channel wasn't found";

struct Job {
  std::string channel;  // the login, lower case
  std::string userId;   // who was logged in when it was asked for
};

struct State {
  // Shared with the worker, under g_mx.
  bool queued = false;     // `job` waits for the worker
  Job job;
  bool busy = false;       // the worker holds one
  bool hasResult = false;  // `result` waits for clip_take_result()
  ClipResult result;

  // Main thread only.
  bool started = false;  // the worker was asked for
  bool broken = false;   // no worker: every clip is turned down
};

static_assert(std::is_nothrow_default_constructible<State>::value,
              "State is built on first use and must not throw");
static_assert(std::is_nothrow_move_assignable<Job>::value && std::is_nothrow_move_assignable<ClipResult>::value,
              "the job and the result change hands under the lock");

pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;  // everything shared in State
pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;    // a clip was asked for
std::atomic<bool> g_result{false};                 // a result waits: clip_take_result()'s idle check
alignas(State) unsigned char g_storage[sizeof(State)];
State* g_state = nullptr;  // main thread; the worker gets it as its argument

// Holds g_mx for one scope, also when an exception leaves it.
class Lock {
 public:
  Lock() noexcept { pthread_mutex_lock(&g_mx); }
  ~Lock() { pthread_mutex_unlock(&g_mx); }
  Lock(const Lock&) = delete;
  Lock& operator=(const Lock&) = delete;
};

// ---- Worker ----

// The channels' user ids, newest last. Worker only.
using Ids = std::vector<std::pair<std::string, std::string>>;

const std::string* findId(const Ids& ids, const std::string& login) noexcept {
  for (const auto& entry : ids)
    if (entry.first == login) return &entry.second;
  return nullptr;
}

void keepId(Ids& ids, const std::string& login, const std::string& id) {
  if (ids.size() >= kMaxIds) ids.erase(ids.begin());
  ids.emplace_back(login, id);
}

// An answer other than 2xx or 401, in the user's words. Twitch's documented
// ones: 403 when the channel doesn't allow clips, 404 when it isn't live.
const char* statusReason(int status) noexcept {
  if (status == 403) return kWhyNotAllowed;
  if (status == 404) return kEvoTwitchOffline;
  if (status == 429 || status >= 500) return kWhyTryAgain;
  return kWhyRefused;
}

// evoTwitchRun's sentence for the user, as a clip's.
const char* runReason(const char* user, const char* why) noexcept {
  if (strcmp(user, kEvoTwitchNoConnection) == 0)
    return strstr(why, "date and time") ? kWhyClock : kEvoTwitchNoConnection;
  if (strcmp(user, kEvoTwitchNotFound) == 0) return kWhyNotFound;
  if (strcmp(user, kEvoTwitchBadAnswer) == 0) return kEvoTwitchBadAnswer;
  return kEvoTwitchFailed;
}

// One clip: made (out->id) or why not (out->error). Logs the outcome, never
// the token.
void makeOne(const Job& job, Ids& ids, ClipResult* out) {
  const unsigned long long t0 = data_now_ms();
  std::string refusedToken;  // the token Twitch turned down on the first try
  for (int attempt = 0;; ++attempt) {
    std::string token, userId;
    if (!auth_token(&token, &userId)) {
      evo_log_warn("twitch: clip: none of %s: not logged in", job.channel.c_str());
      out->error = kWhyLoggedOut;
      return;
    }
    if (userId != job.userId) {
      evo_log_warn("twitch: clip: none of %s: another user logged in since it was asked for",
                   job.channel.c_str());
      out->error = kWhyLoggedOut;
      return;
    }
    // The same token again: the refresh got no answer, so the login service
    // hands out the old one until it tries again. Sending it would only be
    // refused once more; the trouble is the network, not the login.
    if (attempt > 0 && token == refusedToken) {
      evo_log_warn("twitch: clip: none of %s after %llu ms: no connection (the refresh got no answer)",
                   job.channel.c_str(), data_now_ms() - t0);
      out->error = kEvoTwitchNoConnection;
      return;
    }

    enum { kNone, kTokenRefused, kScopeMissing, kStatus } failure = kNone;
    int status = 0;
    char detail[256] = "";
    twitch::CreatedClip created;
    char why[256];
    const char* user = evoTwitchRun(
        [&] {
          try {
            twitch::HelixClient helix(evoTwitchHttpVerified(), twitch::kAppClientId, token);
            const std::string* id = findId(ids, job.channel);
            if (!id) {
              const std::string fresh = helix.userIdForLogin(job.channel);
              keepId(ids, job.channel, fresh);
              id = &ids.back().second;
            }
            created = helix.createClip(*id);
          } catch (const twitch::UnauthorizedError& e) {
            failure = e.missingScope() ? kScopeMissing : kTokenRefused;
            snprintf(detail, sizeof detail, "%s", e.what());
          } catch (const twitch::HttpStatusError& e) {
            failure = kStatus;
            status = e.status();
            snprintf(detail, sizeof detail, "%s", e.what());
          }
        },
        why, sizeof why);
    const unsigned long long ms = data_now_ms() - t0;

    if (failure == kTokenRefused) {
      // Reported every time, so the login service refreshes (or logs out)
      // whichever token it was; asked for once more only.
      auth_token_rejected(token);
      if (attempt == 0) {
        evo_log_warn("twitch: clip: Twitch refused the token (%s), trying again after the refresh", detail);
        refusedToken.swap(token);
        continue;
      }
    }
    const char* fixed = nullptr;
    if (failure == kTokenRefused || failure == kScopeMissing)
      fixed = kWhyLogInAgain;  // a missing scope only comes with a new login
    else if (failure == kStatus)
      fixed = statusReason(status);
    else if (user)
      fixed = runReason(user, why);
    if (fixed) {
      evo_log_warn("twitch: clip: none of %s after %llu ms: %s (%s)", job.channel.c_str(), ms, fixed,
                   failure != kNone ? detail : why);
      out->error = fixed;
      return;
    }
    out->made = true;
    out->id = created.id;
    evo_log("twitch: clip: made %s of %s in %llu ms", created.id.c_str(), job.channel.c_str(), ms);
    return;
  }
}

void* worker(void* arg) noexcept {
  State& s = *static_cast<State*>(arg);
  // Before any core call (evo_twitch_internal.h). Without working exceptions
  // this thread can't run the core: every clip then fails.
  const bool ok = evoTwitchWorkerCheck();
  if (!ok) evo_log_error("twitch: clip: clips off, the core can't run on its thread");
  Ids ids;
  for (;;) {
    Job job;
    {
      Lock lock;
      while (!s.queued) pthread_cond_wait(&g_cv, &g_mx);
      job = std::move(s.job);
      s.queued = false;
      s.busy = true;
    }
    ClipResult result;
    try {
      result.channel = job.channel;
      if (ok)
        makeOne(job, ids, &result);
      else
        result.error = kRefusedNoWorker;
    } catch (...) {
      // Out of memory: no clip, and an empty error, which clip_take_result()
      // turns into "Something went wrong".
      evo_log_error("twitch: clip: out of memory while making a clip of %s", job.channel.c_str());
      result.made = false;
      result.id.clear();
      result.error.clear();
    }
    Lock lock;
    s.result = std::move(result);
    s.hasResult = true;
    s.busy = false;
    g_result.store(true, std::memory_order_release);
  }
  return nullptr;
}

// ---- Main thread ----

State& state() noexcept {
  if (!g_state) g_state = new (g_storage) State();
  return *g_state;
}

// The worker, started once. False when there is none.
bool startWorker(State& s) noexcept {
  if (s.started) return !s.broken;
  s.started = true;
  int selftest = evoTwitchSelftestState();
  if (selftest < 0) selftest = evo_twitch_selftest();
  if (selftest != 1) {
    evo_log_error("twitch: clip: clips off because the self-test failed");
    s.broken = true;
    return false;
  }
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, kStackBytes);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_t thread;
  const int rc = pthread_create(&thread, &attr, worker, &s);
  pthread_attr_destroy(&attr);
  if (rc != 0) {
    evo_log_error("twitch: clip worker: pthread_create rc=%d", rc);
    s.broken = true;
    return false;
  }
  evo_log("twitch: clip: worker up");
  return true;
}

char lowerAscii(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

}  // namespace

const char* clip_make(const std::string& channel) noexcept {
  try {
    const AuthView& auth = auth_view();
    if (auth.state == AuthState::Unknown) return kRefusedChecking;  // auth.json isn't read yet
    if (auth.state != AuthState::LoggedIn || auth.userId.empty()) return kRefusedLoggedOut;
    if (!auth_has_scope(auth, twitch::kClipScope)) {
      evo_log_warn("twitch: clip: the login has no %s, a new login is needed", twitch::kClipScope);
      return kRefusedScope;
    }
    if (channel.empty()) return kEvoTwitchFailed;
    State& s = state();
    if (!startWorker(s)) return kRefusedNoWorker;

    Job job;
    job.channel = channel;
    for (char& c : job.channel) c = lowerAscii(c);
    job.userId = auth.userId;
    Lock lock;
    if (s.queued || s.busy || s.hasResult) return kRefusedBusy;
    s.job = std::move(job);
    s.queued = true;
    pthread_cond_signal(&g_cv);
    return nullptr;
  } catch (...) {
    evo_log_error("twitch: clip: could not ask for a clip");
    return kEvoTwitchFailed;
  }
}

bool clip_take_result(ClipResult* out) noexcept {
  if (!g_result.load(std::memory_order_acquire) || !g_state) return false;
  try {
    ClipResult result;
    {
      Lock lock;
      State& s = *g_state;
      if (!s.hasResult) return false;
      result = std::move(s.result);
      s.hasResult = false;
      g_result.store(false, std::memory_order_release);
    }
    if (!result.made && result.error.empty()) result.error = kEvoTwitchFailed;
    *out = std::move(result);
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace evo_twitch
