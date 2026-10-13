// Sending chat messages as the logged-in user. The contract is in
// evo_twitch_chat.h (chat_send, chat_take_send_notice).
//
//   main thread                                    send worker (1, started by the first message)
//   chat_send(channel, text)
//     blank            -> nothing
//     logged out, or kChatSendQueueMax
//     already on their way -> the toast, at once
//     otherwise        -> Job --queue (g_mx)------> one at a time, oldest first:
//                                                   auth_token() (may wait for a refresh),
//                                                   the channel's user id (cached),
//                                                   POST /chat/messages;
//                                                   a 401 for the token: reported, and
//                                                   tried once more after the refresh
//   chat_take_send_notice() <--notices (g_mx)----- a failure: "Message not sent: <why>"
//                                                   (success: nothing, the message
//                                                   comes back through chat); when
//                                                   Twitch gave no reason of its own,
//                                                   the chat's mode, if one is on
//                                                   (chat_room_explains())
//   chat_take_draft()       <--draft (g_mx)------- and the failed text, kept for
//                                                   kChatDraftKeepMs (a refusal in
//                                                   chat_send() keeps it too)
//
// Why a thread of its own and not the chat or the data worker: a send can
// wait up to 10 s for a token refresh and then for Twitch. On the chat worker,
// which polls its connection every 250 ms, that would freeze chat on screen;
// on the data worker it would hold up the pages behind it, and that queue
// keeps only the newest request per slot, where every message counts. The
// thread sleeps on its condvar while nothing is queued, and a user who never
// sends never starts it.
//
// File-scope state is trivially constructible, like evo_twitch_data.cpp: the
// C++ objects are placement-constructed into static storage on first use and
// never destroyed.

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
#include "evo_twitch_chat.h"
#include "evo_twitch_data.h"
#include "evo_twitch_internal.h"
#include "twitch/app.hpp"
#include "twitch/error.hpp"
#include "twitch/helix.hpp"

namespace evo_twitch {
namespace {

constexpr size_t kStackBytes = 2 * 1024 * 1024;  // TLS with certificate checks, as the other workers
constexpr size_t kMaxNotices = 8;                // not taken yet; older ones go first
constexpr size_t kMaxIds = 32;                   // channels whose user id is remembered
constexpr size_t kMaxReason = 160;               // bytes of Twitch's reason in a toast

// The toasts chat_send() hands back.
const char* const kRefusedLoggedOut = "Message not sent: log in under Settings to chat";
const char* const kRefusedFull = "Message not sent: wait for the last ones to go out";
const char* const kRefusedNoWorker = "Message not sent: sending isn't available in this build";
const char* const kRefusedFailed = "Message not sent: something went wrong";

// The worker's reasons, after "Message not sent: ".
const char* const kWhyLoggedOut = "log in under Settings to chat";
const char* const kWhySwitched = "you switched Twitch accounts";
const char* const kWhyLogInAgain = "log in again under Settings";
const char* const kWhyNoConnection = "no connection";
const char* const kWhyClock = "check the PS5's date and time";
const char* const kWhyNotFound = "this channel wasn't found";
const char* const kWhyForbidden = "you can't chat in this channel";
const char* const kWhyTooLong = "the message is too long";
const char* const kWhyTooFast = "too many messages, wait a moment";
const char* const kWhyRejected = "Twitch didn't take it";
const char* const kWhyTwitchDown = "Twitch had a problem, try again";
const char* const kWhyBadAnswer = "Twitch didn't answer as expected";
const char* const kWhyFailed = "something went wrong";
const char* const kWhyHeldBack = "Twitch held it back";
const char* const kWhyNoWorker = "sending isn't available in this build";

struct Job {
  std::string channel;  // the login, lower case
  std::string userId;   // who was logged in when it was queued
  std::string text;
};

// A failure for the main thread to toast.
struct Notice {
  std::string text;     // "Message not sent: <why>"
  std::string channel;  // the login it was for
  bool vague = false;   // Twitch dropped it without saying why: the chat's mode may tell
};

struct State {
  // Shared with the worker, under g_mx.
  std::vector<Job> queue;            // waiting, oldest first
  bool busy = false;                 // the worker holds a job
  std::vector<Notice> notices;       // failures not taken yet, oldest first
  // The last message that wasn't sent, for chat_take_draft().
  bool hasDraft = false;
  std::string draftChannel;          // lower case
  std::string draftText;
  unsigned long long draftAtMs = 0;  // when it went wrong, on data_now_ms()'s clock

  // Main thread only.
  bool started = false;  // the worker was asked for
  bool broken = false;   // no worker: every message is refused
};

static_assert(std::is_nothrow_default_constructible<State>::value,
              "State is built on first use and must not throw");
static_assert(std::is_nothrow_move_assignable<Job>::value && std::is_nothrow_move_constructible<Job>::value,
              "the worker takes a job off the queue under the lock");

pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;  // queue, busy, notices
pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;    // a job was queued
std::atomic<int> g_notices{0};                     // notices waiting: chat_take_send_notice()'s idle check
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

// Twitch's reason as a toast can take it: one line, cut at a character
// boundary.
std::string tidyReason(const std::string& in) {
  std::string out = in.substr(0, kMaxReason);
  if (out.size() < in.size()) {
    size_t end = out.size();
    while (end > 0 && (static_cast<unsigned char>(in[end]) & 0xC0) == 0x80) --end;
    out.resize(end);
    out += "...";
  }
  for (char& c : out)
    if (static_cast<unsigned char>(c) < 0x20) c = ' ';
  return out;
}

// An answer other than 2xx or 401, in the user's words.
const char* statusReason(int status) noexcept {
  if (status == 403) return kWhyForbidden;
  if (status == 422) return kWhyTooLong;
  if (status == 429) return kWhyTooFast;
  if (status == 400) return kWhyRejected;
  if (status >= 500) return kWhyTwitchDown;
  return kWhyBadAnswer;
}

// evoTwitchRun's sentence for the user, as a reason after "Message not sent: ".
const char* runReason(const char* user, const char* why) noexcept {
  if (strcmp(user, kEvoTwitchNoConnection) == 0)
    return strstr(why, "date and time") ? kWhyClock : kWhyNoConnection;
  if (strcmp(user, kEvoTwitchNotFound) == 0) return kWhyNotFound;
  if (strcmp(user, kEvoTwitchBadAnswer) == 0) return kWhyBadAnswer;
  return kWhyFailed;
}

// One message. True when Twitch took it; otherwise *reason says why, for
// after "Message not sent: ", and *vague is set when that is ours because
// Twitch gave none (it dropped the message without a reason, or answered 400).
// Logs the outcome, never the text.
bool sendOne(const Job& job, Ids& ids, std::string* reason, bool* vague) {
  const unsigned long long t0 = data_now_ms();
  std::string refusedToken;  // the token Twitch turned down on the first try
  for (int attempt = 0;; ++attempt) {
    std::string token, userId;
    if (!auth_token(&token, &userId)) {
      evo_log_warn("twitch: chat: not sent to %s: not logged in", job.channel.c_str());
      *reason = kWhyLoggedOut;
      return false;
    }
    if (userId != job.userId) {
      evo_log_warn("twitch: chat: not sent to %s: another user logged in since it was written",
                   job.channel.c_str());
      *reason = kWhySwitched;
      return false;
    }
    // The same token again: the refresh got no answer, so the login service
    // hands out the old one until it tries again. Sending it would only be
    // refused once more, and the trouble is the network, not the login.
    if (attempt > 0 && token == refusedToken) {
      evo_log_warn("twitch: chat: not sent to %s after %llu ms: no connection (the refresh got no answer)",
                   job.channel.c_str(), data_now_ms() - t0);
      *reason = kWhyNoConnection;
      return false;
    }

    enum { kNone, kTokenRefused, kScopeMissing, kStatus } failure = kNone;
    int status = 0;
    char detail[256] = "";
    twitch::SendResult result;
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
            result = helix.sendChat(*id, userId, job.text);
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
      // whichever token it was; tried once more only.
      auth_token_rejected(token);
      if (attempt == 0) {
        evo_log_warn("twitch: chat: Twitch refused the token (%s), trying again after the refresh", detail);
        refusedToken.swap(token);
        continue;
      }
    }
    const char* fixed = nullptr;
    if (failure == kTokenRefused || failure == kScopeMissing)
      fixed = kWhyLogInAgain;  // a missing scope only comes back with a new login
    else if (failure == kStatus)
      fixed = statusReason(status);
    else if (user)
      fixed = runReason(user, why);
    if (fixed) {
      evo_log_warn("twitch: chat: not sent to %s after %llu ms: %s (%s)", job.channel.c_str(), ms, fixed,
                   failure != kNone ? detail : why);
      *reason = fixed;
      *vague = fixed == kWhyRejected;
      return false;
    }
    if (!result.sent) {
      if (result.dropReason.empty()) {
        *reason = kWhyHeldBack;
        *vague = true;
        evo_log_warn("twitch: chat: not sent to %s after %llu ms: Twitch dropped it without a reason",
                     job.channel.c_str(), ms);
      } else {
        *reason = tidyReason(result.dropReason);
        evo_log_warn("twitch: chat: not sent to %s after %llu ms: Twitch dropped it: %s", job.channel.c_str(), ms,
                     reason->c_str());
      }
      return false;
    }
    evo_log("twitch: chat: sent to %s in %llu ms", job.channel.c_str(), ms);
    return true;
  }
}

// Under g_mx: a notice for the main thread. The same as the one waiting last
// is left out (five messages lost to one dead network make one toast), and
// the oldest goes when nobody has taken them for a while.
void postNotice(State& s, Notice&& notice) {
  if (!s.notices.empty() && s.notices.back().text == notice.text && s.notices.back().channel == notice.channel)
    return;
  if (s.notices.size() >= kMaxNotices) s.notices.erase(s.notices.begin());
  s.notices.push_back(std::move(notice));
  g_notices.store(static_cast<int>(s.notices.size()), std::memory_order_release);
}

// Under g_mx: text becomes the draft for channel. Swapped in, so it can't
// fail; text is left with the old draft.
void keepDraft(State& s, std::string& channel, std::string& text) noexcept {
  s.draftChannel.swap(channel);
  s.draftText.swap(text);
  s.draftAtMs = data_now_ms();
  s.hasDraft = true;
}

void* worker(void* arg) noexcept {
  State& s = *static_cast<State*>(arg);
  // Before any core call (evo_twitch_internal.h). Without working exceptions
  // this thread can't run the core: every message then fails with a notice.
  const bool ok = evoTwitchWorkerCheck();
  if (!ok) evo_log_error("twitch: chat: sending off, the core can't run on its thread");
  Ids ids;
  for (;;) {
    Job job;
    {
      Lock lock;
      s.busy = false;
      while (s.queue.empty()) pthread_cond_wait(&g_cv, &g_mx);
      job = std::move(s.queue.front());
      s.queue.erase(s.queue.begin());
      s.busy = true;
    }
    try {
      std::string reason;
      bool vague = false;
      const bool sent = ok ? sendOne(job, ids, &reason, &vague) : false;
      if (!ok) reason = kWhyNoWorker;
      if (!sent) {
        Notice notice;
        notice.text = "Message not sent: " + reason;
        notice.channel = job.channel;
        notice.vague = vague;
        std::string channel = job.channel;
        Lock lock;
        postNotice(s, std::move(notice));
        keepDraft(s, channel, job.text);
      }
    } catch (...) {
      // Out of memory: the message's outcome is lost, the next one is tried.
      evo_log_error("twitch: chat: out of memory while sending to %s", job.channel.c_str());
    }
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
    evo_log_error("twitch: chat: sending off because the self-test failed");
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
    evo_log_error("twitch: chat send worker: pthread_create rc=%d", rc);
    s.broken = true;
    return false;
  }
  evo_log("twitch: chat: send worker up");
  return true;
}

char lowerAscii(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool blank(const std::string& text) noexcept {
  for (const char c : text)
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') return false;
  return true;
}

}  // namespace

const char* chat_send(const std::string& channel, const std::string& text) noexcept {
  try {
    if (blank(text)) return nullptr;
    State& s = state();

    Job job;
    job.channel = channel;
    for (char& c : job.channel) c = lowerAscii(c);
    job.text = text;
    // One line: the IME's own keyboard can't type a line break, but a pasted
    // or phone-sent text could carry one.
    for (char& c : job.text)
      if (c == '\r' || c == '\n' || c == '\t') c = ' ';

    // Refused here: the text is kept for the next keyboard all the same.
    const char* refused = nullptr;
    const AuthView& auth = auth_view();
    if (auth.state != AuthState::LoggedIn || auth.userId.empty()) {
      refused = kRefusedLoggedOut;
    } else if (!startWorker(s)) {
      refused = kRefusedNoWorker;
    } else {
      job.userId = auth.userId;
      Lock lock;
      if (s.queue.size() + (s.busy ? 1 : 0) >= static_cast<size_t>(kChatSendQueueMax)) {
        refused = kRefusedFull;
      } else {
        s.queue.push_back(std::move(job));
        pthread_cond_signal(&g_cv);
        return nullptr;
      }
    }
    if (refused == kRefusedFull)
      evo_log_warn("twitch: chat: not sent to %s: %d messages already on their way", job.channel.c_str(),
                   kChatSendQueueMax);
    Lock lock;
    keepDraft(s, job.channel, job.text);
    return refused;
  } catch (...) {
    evo_log_error("twitch: chat: could not queue a message");
    return kRefusedFailed;
  }
}

bool chat_peek_draft(const std::string& channel, std::string* out) noexcept {
  if (!g_state) return false;
  try {
    std::string want = channel;
    for (char& c : want) c = lowerAscii(c);
    Lock lock;
    State& s = *g_state;
    if (!s.hasDraft) return false;
    if (data_now_ms() - s.draftAtMs >= kChatDraftKeepMs) {
      s.hasDraft = false;  // too old for any channel: dropped
      s.draftText.clear();
      return false;
    }
    if (s.draftChannel != want) return false;  // kept for its own channel
    *out = s.draftText;
    return true;
  } catch (...) {
    return false;
  }
}

bool chat_take_draft(const std::string& channel, std::string* out) noexcept {
  if (!g_state) return false;
  try {
    std::string want = channel;
    for (char& c : want) c = lowerAscii(c);
    std::string text;
    {
      Lock lock;
      State& s = *g_state;
      if (!s.hasDraft) return false;
      if (data_now_ms() - s.draftAtMs >= kChatDraftKeepMs) {
        s.hasDraft = false;  // too old for any channel: dropped
        s.draftText.clear();
        return false;
      }
      if (s.draftChannel != want) return false;  // kept for its own channel
      s.hasDraft = false;
      text.swap(s.draftText);
    }
    out->swap(text);
    return true;
  } catch (...) {
    return false;
  }
}

bool chat_take_send_notice(std::string* out) noexcept {
  if (g_notices.load(std::memory_order_acquire) == 0 || !g_state) return false;
  try {
    Notice notice;
    {
      Lock lock;
      State& s = *g_state;
      if (s.notices.empty()) return false;
      notice = std::move(s.notices.front());
      s.notices.erase(s.notices.begin());
      g_notices.store(static_cast<int>(s.notices.size()), std::memory_order_release);
    }
    // Twitch didn't say why: the chat's mode (subscribers-only, slow ...)
    // usually does.
    std::string mode;
    if (notice.vague && chat_room_explains(notice.channel, &mode)) notice.text = "Message not sent: " + mode;
    out->swap(notice.text);
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace evo_twitch
