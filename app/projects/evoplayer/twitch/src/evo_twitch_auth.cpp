// Login service. The contract is in evo_twitch_auth.h.
//
//   main thread                     any thread              worker (1)
//   auth_begin_login/cancel/logout                          device flow, revoke,
//     epoch++, flags, view  --g_mx, g_work_cv-------------> auth.json (read, write,
//   auth_frame(): the path                                  delete), validate,
//     once the sandbox opens --------------------------->   refresh
//                                   auth_token()
//                                     refreshWanted ----->  the refresh token's
//                                     waits on g_token_cv <- only owner: new
//                                                            access token, expiry
//   auth_frame(): view copy  <------------------------------ view + g_view_seq
//
// The refresh token lives only in the worker (Worker::login_). Twitch takes
// each one once, so nothing else may ever send it: other threads get the
// access token and the user id, and ask for a refresh through refreshWanted.
// However many ask, the worker runs one refresh at a time, and a request for
// a token that has already been replaced is ignored.
//
// Epochs. Every login, cancel and logout bumps s.epoch, and so does a logout
// the worker makes when Twitch refuses the refresh. The worker notes the epoch
// when it takes a task and applies the outcome only if it hasn't moved, so a
// login, refresh or file read that finishes after the user moved on is
// dropped (and any tokens it got are revoked). That is what lets the main
// thread change the view at once, without waiting for a request in flight.
//
// Durability. A new pair (login or refresh) is saved to auth.json first, but
// the save can fail, and the new tokens are handed out either way; a crash
// before a failed save finally succeeds can leave auth.json holding the old,
// already-spent refresh token. A save that fails is retried (markDirty, then
// next()) until it works. A session that ends loses its file first and is
// only then revoked on the network (both tokens), so a logout holds even if
// the console goes off halfway.
//
// File-scope state is trivially constructible, like evo_twitch_data.cpp: the
// state is placement-constructed into static storage on first use and never
// destroyed.

#include "evo_twitch_auth.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <new>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "evo_boot_log.h"
#include "evo_data_path.h"
#include "evo_jailbreak.h"
#include "evo_twitch.h"
#include "evo_twitch_data.h"
#include "evo_twitch_internal.h"
#include "twitch/app.hpp"
#include "twitch/auth.hpp"
#include "twitch/error.hpp"
#include "twitch/helix.hpp"

// The host test (twitch/tests/auth_host_test.sh) builds with shorter times and
// says itself when the sandbox opens; the app keeps these.
#ifdef EVO_TWITCH_AUTH_OPEN_HOOK
int EVO_TWITCH_AUTH_OPEN_HOOK() noexcept;
#endif
#ifndef EVO_TWITCH_AUTH_SECOND_MS
#define EVO_TWITCH_AUTH_SECOND_MS 1000  // one of Twitch's seconds in the device flow
#endif
#ifndef EVO_TWITCH_AUTH_VALIDATE_MS
#define EVO_TWITCH_AUTH_VALIDATE_MS 3600000  // Twitch asks for one validation an hour
#endif
#ifndef EVO_TWITCH_AUTH_RETRY_MS
#define EVO_TWITCH_AUTH_RETRY_MS 120000  // the next validation after one without an answer
#endif
#ifndef EVO_TWITCH_AUTH_BACKOFF_MS
#define EVO_TWITCH_AUTH_BACKOFF_MS 5000  // the first pause after a refresh without an answer
#endif
#ifndef EVO_TWITCH_AUTH_SAVE_RETRY_MS
#define EVO_TWITCH_AUTH_SAVE_RETRY_MS 1000  // the first wait before saving auth.json again
#endif

namespace evo_twitch {
namespace {

constexpr size_t kStackBytes = 2 * 1024 * 1024;  // as the data service's worker
constexpr unsigned long long kSecondMs = EVO_TWITCH_AUTH_SECOND_MS;
constexpr unsigned long long kValidateMs = EVO_TWITCH_AUTH_VALIDATE_MS;
constexpr unsigned long long kRetryMs = EVO_TWITCH_AUTH_RETRY_MS;
constexpr unsigned long long kBackoffMs = EVO_TWITCH_AUTH_BACKOFF_MS;
constexpr unsigned long long kMaxBackoffMs = 5 * 60 * 1000;
constexpr unsigned long long kSaveRetryMs = EVO_TWITCH_AUTH_SAVE_RETRY_MS;
constexpr unsigned long long kMaxSaveRetryMs = 30000;
constexpr long long kAheadMs = 5 * 60 * 1000;          // refresh when less than this is left
constexpr long long kUnknownLifeMs = 60 * 60 * 1000;   // a new token whose lifetime Twitch didn't give
constexpr unsigned long long kTokenWaitMs = 10000;     // auth_token()'s longest wait for a refresh
constexpr unsigned long long kOpenCheckMs = 250;       // how often auth_frame() looks for the sandbox
constexpr unsigned long long kLongWaitMs = 60000;      // longest single wait on a monotonic condvar
constexpr unsigned long long kSliceMs = 1000;          // the same on the wall-clock fallback
constexpr int kMeTries = 3;
constexpr int kMaxPollFailures = 3;
constexpr size_t kMaxFileBytes = 64 * 1024;
constexpr const char* kAuthFile = "auth.json";
constexpr const char* kAuthTmpFile = "auth.json.tmp";
constexpr const char* kBadSuffix = ".bad";  // an unreadable auth.json is moved aside to auth.json.bad

// What the view says.
const char* const kRanOut = "Your Twitch login ran out. Log in again.";
const char* const kNotInBuild = "Logging in isn't available in this build.";
const char* const kErrOffline = "Couldn't reach Twitch. Check the connection and try again.";
const char* const kErrClock = "Couldn't check Twitch's certificate. Check the PS5's date and time.";
const char* const kErrBadAnswer = "Twitch didn't answer as expected. Try again.";
const char* const kErrExpired = "The code ran out before it was used. Try again for a new one.";
const char* const kErrDenied = "The login was turned down on Twitch.";
const char* const kErrNoUser = "Twitch didn't say whose account this is. Try again.";

struct State {
  // Shared, under g_mx.
  bool loggedIn = false;  // auth_token() hands out access and userId
  std::string access;
  std::string userId;
  long long expiresAtMs = 0;           // wall clock, as auth.json keeps it
  bool refreshing = false;             // the worker is inside a refresh
  bool refreshWanted = false;          // auth_token(), a 401 or a refused validation asked for one
  unsigned long long pauseUntil = 0;   // monotonic: a refresh just went unanswered, none before this
  unsigned epoch = 0;                  // see the top of the file
  bool loginWanted = false;            // a device flow is wanted, from begin until it ends
  bool logoutWanted = false;           // the worker still has to revoke and delete
  bool dropFile = false;               // logged out: auth.json, if not read yet, is deleted unread
  bool pathReady = false;              // path and tmpPath are set
  std::string path;
  std::string tmpPath;
  bool dead = false;                   // no worker: Error for good
  AuthView view;                       // the truth; auth_frame() copies it
  std::string notice;

  // Main thread only.
  AuthView shown;  // what auth_view() returns
  unsigned shownSeq = 0;
  unsigned long long nextOpenCheck = 0;
  bool pathSent = false;
  bool started = false;
};

// auth_view() hands out a reference, so building the state must not fail.
static_assert(std::is_nothrow_default_constructible<State>::value,
              "State is built on first use and must not throw");

pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;  // everything shared in State
// Set up by auth_start() (initCondvars) before the worker exists. Other
// threads only touch them once loggedIn is true, which only the worker sets.
pthread_cond_t g_work_cv;   // something for the worker to do
pthread_cond_t g_token_cv;  // a refresh ended or the session changed: auth_token() looks again
clockid_t g_cv_clock = CLOCK_REALTIME;  // the clock of both condvars; fixed before the worker starts
bool g_cv_ready = false;                // main thread: the condvars are set up
std::atomic<bool> g_sleep_waits{false};  // timed waits came back early: everyone sleeps instead
std::atomic<unsigned> g_view_seq{0};     // bumped under g_mx with every change of view or notice
std::atomic<State*> g_state{nullptr};    // created by the main thread; other threads only read it
pthread_t g_main_thread;                 // set by auth_start(), then published by g_main_known
std::atomic<bool> g_main_known{false};
std::atomic<bool> g_main_warned{false};  // auth_token() on the main thread was logged
alignas(State) unsigned char g_storage[sizeof(State)];

// Holds g_mx for one scope, also when an exception leaves it.
class Lock {
 public:
  Lock() noexcept { pthread_mutex_lock(&g_mx); }
  ~Lock() { pthread_mutex_unlock(&g_mx); }
  Lock(const Lock&) = delete;
  Lock& operator=(const Lock&) = delete;
};

// Main thread.
State& state() noexcept {
  State* s = g_state.load(std::memory_order_relaxed);
  if (!s) {
    s = new (g_storage) State();
    g_state.store(s, std::memory_order_release);
  }
  return *s;
}

unsigned long long monoMs() noexcept { return data_now_ms(); }

// Token expiry is wall-clock time, so it means the same after a restart.
long long wallMs() noexcept {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<long long>(ts.tv_sec) * 1000LL + ts.tv_nsec / 1000000;
}

// Under g_mx.
void viewChanged() noexcept { g_view_seq.fetch_add(1, std::memory_order_release); }

bool sandboxOpen() noexcept {
#ifdef EVO_TWITCH_AUTH_OPEN_HOOK
  return EVO_TWITCH_AUTH_OPEN_HOOK() != 0;
#else
  return evo_jailbreak_is_open() != 0;
#endif
}

// Main thread, before the worker starts: both condvars with their timed waits
// on CLOCK_MONOTONIC, falling back to the wall clock where that isn't offered
// (as evo_twitch_chat.cpp). False when there is no condvar at all.
bool initCondvars() noexcept {
#ifdef EVO_TWITCH_AUTH_TEST_WRONG_CLOCK
  // Host test only: wall-clock condvars given monotonic deadlines, so every
  // timed wait returns at once, as on a console that ignored the clock
  // attribute. waitOn() must notice.
  g_cv_clock = CLOCK_MONOTONIC;
  return pthread_cond_init(&g_work_cv, nullptr) == 0 && pthread_cond_init(&g_token_cv, nullptr) == 0;
#endif
  pthread_condattr_t attr;
  if (pthread_condattr_init(&attr) == 0) {
    bool ok = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) == 0 &&
              pthread_cond_init(&g_work_cv, &attr) == 0;
    if (ok && pthread_cond_init(&g_token_cv, &attr) != 0) {
      pthread_cond_destroy(&g_work_cv);
      ok = false;
    }
    pthread_condattr_destroy(&attr);
    if (ok) {
      g_cv_clock = CLOCK_MONOTONIC;
      return true;
    }
  }
  if (pthread_cond_init(&g_work_cv, nullptr) != 0) return false;
  if (pthread_cond_init(&g_token_cv, nullptr) != 0) return false;
  g_cv_clock = CLOCK_REALTIME;
  evo_log_warn("twitch: auth: no monotonic condvar, timed waits use the wall clock");
  return true;
}

// Under g_mx: waits on cv for up to ms, or until it is signalled. Callers
// look at the state again after every call. The spin guard is the chat's: a
// timeout that comes back before half the time has passed means the condvar
// ignores its clock, and from then on every wait is a plain sleep of up to
// 100 ms with the lock released.
void waitOn(pthread_cond_t* cv, unsigned long long ms) noexcept {
  const unsigned long long cap = g_cv_clock == CLOCK_MONOTONIC ? kLongWaitMs : kSliceMs;
  if (ms > cap) ms = cap;
  if (ms == 0) ms = 1;
  if (g_sleep_waits.load(std::memory_order_relaxed)) {
    const unsigned long long nap = ms < 100 ? ms : 100;
    struct timespec t;
    t.tv_sec = 0;
    t.tv_nsec = static_cast<long>(nap) * 1000000L;
    pthread_mutex_unlock(&g_mx);
    nanosleep(&t, nullptr);
    pthread_mutex_lock(&g_mx);
    return;
  }
  const unsigned long long start = monoMs();
  struct timespec until;
  clock_gettime(g_cv_clock, &until);
  until.tv_sec += static_cast<time_t>(ms / 1000);
  until.tv_nsec += static_cast<long>(ms % 1000) * 1000000L;
  if (until.tv_nsec >= 1000000000L) {
    until.tv_sec++;
    until.tv_nsec -= 1000000000L;
  }
  const int rc = pthread_cond_timedwait(cv, &g_mx, &until);
  // Under 20 ms the millisecond clock is too coarse to tell.
  if (rc == ETIMEDOUT && ms >= 20 && monoMs() - start < ms / 2 && !g_sleep_waits.exchange(true)) {
    pthread_mutex_unlock(&g_mx);  // not while other threads may want the lock
    evo_log_warn("twitch: auth: timed waits return early, sleeping in 100 ms steps instead");
    pthread_mutex_lock(&g_mx);
  }
}

enum class Outcome { Ok, Refused, Offline, Failed };

// A core call through evoTwitchRun. A refused token (UnauthorizedError) is
// caught inside, so it can be told apart from everything else. *user gets
// evoTwitchRun's sentence (nullptr when it went through or was refused).
template <class Fn>
Outcome callCore(Fn&& fn, char* why, size_t cap, const char** user = nullptr) noexcept {
  bool refused = false;
  const char* u = evoTwitchRun(
      [&] {
        try {
          fn();
        } catch (const twitch::UnauthorizedError& e) {
          refused = true;
          snprintf(why, cap, "%s", e.what());
        }
      },
      why, cap);
  if (user) *user = u;
  if (refused) return Outcome::Refused;
  if (!u) return Outcome::Ok;
  return u == kEvoTwitchNoConnection ? Outcome::Offline : Outcome::Failed;
}

// The sentence for a request that went wrong. A certificate refused for its
// dates is nearly always the console's clock.
const char* sentenceFor(const char* user, const char* why) noexcept {
  if (strstr(why, "date and time")) return kErrClock;
  return user == kEvoTwitchNoConnection ? kErrOffline : kErrBadAnswer;
}

// A new token whose lifetime Twitch left out (or gave as nearly nothing)
// would otherwise look due for a refresh at once, again and again. It gets an
// hour; the hourly validation or a 401 puts that right.
void giveLifetime(twitch::Tokens& t) noexcept {
  const long long now = wallMs();
  if (t.expiresAtMs - now < kAheadMs + 60000) t.expiresAtMs = now + kUnknownLifeMs;
}

// ---- auth.json (worker only) ----

// `path` into `out`, capped at kMaxFileBytes. False when it can't be opened
// (missing, most likely); out is then empty.
bool readFile(const std::string& path, std::string& out) {
  out.clear();
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  char buf[4096];
  size_t n;
  while (out.size() < kMaxFileBytes && (n = fread(buf, 1, sizeof buf, f)) > 0) {
    const size_t room = kMaxFileBytes - out.size();
    out.append(buf, n < room ? n : room);
  }
  fclose(f);
  return true;
}

// Flushes the directory that holds `path`, so a rename or a delete in it
// survives a power cut. Not every file system can; errors are ignored.
void syncDir(const std::string& path) noexcept {
  const size_t slash = path.rfind('/');
  if (slash == std::string::npos) return;
  try {
    const std::string dir = slash == 0 ? std::string("/") : path.substr(0, slash);
#ifdef O_DIRECTORY
    const int fd = open(dir.c_str(), O_RDONLY | O_DIRECTORY);
#else
    const int fd = open(dir.c_str(), O_RDONLY);
#endif
    if (fd < 0) return;
    fsync(fd);
    close(fd);
  } catch (...) {
  }
}

// data to a .tmp file only the owner can read, flushed to the disk, then
// renamed over path (with evo_twitch_settings.cpp's one retry).
bool writeFile(const std::string& path, const std::string& tmp, const std::string& data) noexcept {
  remove(tmp.c_str());  // a leftover would keep its old mode through O_TRUNC
  const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    evo_log_warn("twitch: auth: could not open %s (errno %d)", tmp.c_str(), errno);
    return false;
  }
  size_t off = 0;
  while (off < data.size()) {
    const ssize_t n = write(fd, data.data() + off, data.size() - off);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    off += static_cast<size_t>(n);
  }
  const bool full = off == data.size();
  // A file system without fsync says EINVAL or ENOTSUP: nothing to do then.
  if (full && fsync(fd) != 0 && errno != EINVAL && errno != ENOTSUP)
    evo_log_warn("twitch: auth: fsync of %s failed (errno %d)", tmp.c_str(), errno);
  close(fd);
  if (!full) {
    evo_log_warn("twitch: auth: short write to %s", tmp.c_str());
    remove(tmp.c_str());
    return false;
  }
  chmod(tmp.c_str(), 0600);  // whatever the umask made of it
  if (rename(tmp.c_str(), path.c_str()) != 0) {
    remove(path.c_str());
    if (rename(tmp.c_str(), path.c_str()) != 0) {
      evo_log_warn("twitch: auth: could not move %s to %s (errno %d)", tmp.c_str(), path.c_str(), errno);
      remove(tmp.c_str());
      return false;
    }
  }
  syncDir(path);
  return true;
}

// ---- Worker ----

class Worker {
 public:
  explicit Worker(State& s) noexcept : s_(s) {}
  void run() noexcept;

 private:
  enum class Task { Logout, File, Save, StartLogin, Poll, Refresh, Validate };

  Task next() noexcept;
  void logout();
  void file();
  void startLogin();
  void poll();
  void finishLogin(twitch::Tokens& t);
  void failLogin(const char* sentence, const char* why);
  bool flowCurrent() noexcept;
  bool waitFlow(unsigned long long ms) noexcept;
  void refresh();
  void validate();
  void takeScopes(std::vector<std::string> scopes) noexcept;
  void revoke(const std::string& token) noexcept;
  void discard(const twitch::Tokens& t) noexcept;
  void revokePending() noexcept;
  void dropSession() noexcept;
  bool save();
  void markDirty() noexcept;
  void moveAside() noexcept;
  void removeFile() noexcept;
  void forget() noexcept;

  State& s_;
  unsigned taskEpoch_ = 0;  // s_.epoch when the task in hand was taken

  // The login. The refresh token is here and nowhere else.
  bool have_ = false;
  twitch::SavedLogin login_;
  unsigned long long nextValidate_ = 0;  // monotonic; 0 = none due
  unsigned long long backoffMs_ = kBackoffMs;
  int validateFails_ = 0;
  std::vector<std::string> pendingRevoke_;  // tokens to revoke once auth.json no longer has them

  // auth.json
  bool fileDone_ = false;  // the path is known and the file has been dealt with
  std::string path_;
  std::string tmpPath_;
  std::string badPath_;
  bool dirty_ = false;                   // login_ isn't on disk yet: save again
  unsigned long long nextSaveAt_ = 0;    // monotonic
  unsigned long long saveBackoffMs_ = kSaveRetryMs;

  // The device flow.
  bool flow_ = false;
  unsigned flowEpoch_ = 0;
  twitch::DeviceCode code_;
  unsigned long long nextPoll_ = 0;
  unsigned long long codeEnd_ = 0;
  unsigned long long intervalMs_ = 0;
  int pollFailures_ = 0;
  bool offlineLogged_ = false;
};

// Waits for the next thing to do, most urgent first: a logout, the file, a
// save that failed before, a login to start, a refresh, a validation, a
// device-flow poll.
Worker::Task Worker::next() noexcept {
  Lock lock;
  for (;;) {
    const unsigned long long now = monoMs();
    taskEpoch_ = s_.epoch;
    if (s_.logoutWanted) {
      s_.logoutWanted = false;
      return Task::Logout;
    }
    if (s_.pathReady && !fileDone_) {
      try {
        path_ = s_.path;
        tmpPath_ = s_.tmpPath;
        badPath_ = s_.path + kBadSuffix;
        fileDone_ = true;
        return Task::File;
      } catch (...) {
        waitOn(&g_work_cv, 1000);  // out of memory: try again in a moment
        continue;
      }
    }
    unsigned long long wait = kLongWaitMs;
    if (dirty_ && have_ && fileDone_) {
      if (now >= nextSaveAt_) return Task::Save;
      wait = nextSaveAt_ - now;
    }
    // A flow the main thread cancelled (or started over) ends here.
    if (flow_ && (!s_.loginWanted || s_.epoch != flowEpoch_)) flow_ = false;
    if (s_.loginWanted && !flow_ && !have_) {
      flow_ = true;
      flowEpoch_ = s_.epoch;
      return Task::StartLogin;
    }

    if (have_) {
      const long long wall = wallMs();
      const long long dueWall = login_.tokens.expiresAtMs - kAheadMs;
      if (s_.refreshWanted || wall >= dueWall) {
        if (now >= s_.pauseUntil) {
          s_.refreshWanted = false;
          s_.refreshing = true;
          return Task::Refresh;
        }
        if (s_.pauseUntil - now < wait) wait = s_.pauseUntil - now;
      } else {
        const unsigned long long in = static_cast<unsigned long long>(dueWall - wall);
        if (in < wait) wait = in;
      }
      if (nextValidate_ != 0) {
        if (now >= nextValidate_) return Task::Validate;
        if (nextValidate_ - now < wait) wait = nextValidate_ - now;
      }
    }
    if (flow_) {
      if (now >= nextPoll_) return Task::Poll;
      if (nextPoll_ - now < wait) wait = nextPoll_ - now;
    }
    waitOn(&g_work_cv, wait);
  }
}

void Worker::run() noexcept {
  for (;;) {
    const Task task = next();
    try {
      switch (task) {
        case Task::Logout: logout(); break;
        case Task::File: file(); break;
        case Task::Save: save(); break;
        case Task::StartLogin: startLogin(); break;
        case Task::Poll: poll(); break;
        case Task::Refresh: refresh(); break;
        case Task::Validate: validate(); break;
      }
    } catch (...) {
      // Out of memory in the middle of a task. Nobody may be left waiting on
      // a refresh that won't end, and the next try comes after a pause.
      evo_log_error("twitch: auth: a task failed for want of memory");
      Lock lock;
      if (s_.refreshing) {
        s_.refreshing = false;
        s_.pauseUntil = monoMs() + backoffMs_;
        pthread_cond_broadcast(&g_token_cv);
      }
      if (task == Task::Validate) nextValidate_ = monoMs() + kRetryMs;
      if (task == Task::Save) markDirty();
      if (task == Task::Poll) nextPoll_ = monoMs() + intervalMs_;
      waitOn(&g_work_cv, 1000);
    }
  }
}

void Worker::revoke(const std::string& token) noexcept {
  if (token.empty()) return;
  char why[256];
  callCore([&] { twitch::AuthClient(evoTwitchHttpVerified(), twitch::kAppClientId).revoke(token); }, why,
           sizeof why);
}

// Queues a pair for revocation: the access token and the refresh token
// (Twitch may refuse the second once the first is gone; that's harmless).
void Worker::discard(const twitch::Tokens& t) noexcept {
  try {
    if (!t.access.empty()) pendingRevoke_.push_back(t.access);
    if (!t.refresh.empty()) pendingRevoke_.push_back(t.refresh);
  } catch (...) {
    // Out of memory: these go unrevoked and simply run out.
  }
}

// Only once auth.json no longer holds any of them.
void Worker::revokePending() noexcept {
  for (const std::string& token : pendingRevoke_) revoke(token);
  pendingRevoke_.clear();
}

// Ends the session on the worker's side: the file goes first, then the
// tokens are revoked over the network.
void Worker::dropSession() noexcept {
  if (have_) discard(login_.tokens);  // a copy: forget() wipes them
  forget();
  removeFile();
  revokePending();
}

void Worker::forget() noexcept {
  have_ = false;
  login_ = twitch::SavedLogin();
  nextValidate_ = 0;
  backoffMs_ = kBackoffMs;
  validateFails_ = 0;
  dirty_ = false;
  saveBackoffMs_ = kSaveRetryMs;
}

void Worker::removeFile() noexcept {
  if (!fileDone_) return;  // s_.dropFile deletes it once the path is known
  if (remove(path_.c_str()) != 0 && errno != ENOENT)
    evo_log_warn("twitch: auth: could not delete %s (errno %d)", path_.c_str(), errno);
  remove(tmpPath_.c_str());
  remove(badPath_.c_str());
  syncDir(path_);
}

// An auth.json that can't be read: out of the way, kept as auth.json.bad for
// a look (deleted if even that fails). Logging out deletes it too.
void Worker::moveAside() noexcept {
  if (rename(path_.c_str(), badPath_.c_str()) == 0) {
    chmod(badPath_.c_str(), 0600);
    evo_log_warn("twitch: auth: %s can't be read, moved it to %s%s", kAuthFile, kAuthFile, kBadSuffix);
  } else {
    remove(path_.c_str());
    evo_log_warn("twitch: auth: %s can't be read, deleted it", kAuthFile);
  }
  syncDir(path_);
}

// Writes login_ to auth.json. A failure marks it dirty: next() tries again
// after a short wait, doubling up to 30 s, until it works.
bool Worker::save() {
  if (!have_ || !fileDone_) return true;  // written once the path is known
  std::string json;
  char why[256];
  if (callCore([&] { json = twitch::serializeLogin(login_); }, why, sizeof why) != Outcome::Ok) {
    evo_log_warn("twitch: auth: could not save the login: %s", why);
    markDirty();
    return false;
  }
  if (!writeFile(path_, tmpPath_, json)) {
    markDirty();
    return false;
  }
  if (dirty_) evo_log("twitch: auth: saved the login after all");
  dirty_ = false;
  saveBackoffMs_ = kSaveRetryMs;
  return true;
}

void Worker::markDirty() noexcept {
  if (!dirty_) evo_log_warn("twitch: auth: the login isn't saved yet, trying again");
  dirty_ = true;
  nextSaveAt_ = monoMs() + saveBackoffMs_;
  saveBackoffMs_ = saveBackoffMs_ * 2 > kMaxSaveRetryMs ? kMaxSaveRetryMs : saveBackoffMs_ * 2;
}

// The main thread has already shown LoggedOut and stopped handing out the
// token (auth_logout()); this is the rest of it.
void Worker::logout() {
  flow_ = false;
  const bool had = have_;
  dropSession();
  if (had) evo_log("twitch: auth: logged out");
}

void Worker::file() {
  if (have_) {  // logged in before the sandbox opened: this login is the newer one
    save();
    return;
  }
  bool drop;
  {
    Lock lock;
    drop = s_.dropFile;
  }
  if (drop) {  // logged out before the sandbox opened
    removeFile();
    return;
  }

  std::string json;
  std::optional<twitch::SavedLogin> saved;
  bool found = false;
  char why[256];
  callCore(
      [&] {
        found = readFile(path_, json);
        saved = twitch::parseLogin(json);
      },
      why, sizeof why);
  if (!saved) {
    {
      Lock lock;
      if (s_.view.state == AuthState::Unknown) {
        s_.view = AuthView();
        s_.view.state = AuthState::LoggedOut;
        viewChanged();
      }
    }
    if (found)
      moveAside();
    else
      evo_log("twitch: auth: no saved login");
    return;
  }

  // Everything that allocates first, then one swap under the lock.
  std::string access = saved->tokens.access;
  std::string userId = saved->userId;
  AuthView v;
  v.state = AuthState::LoggedIn;
  v.login = saved->login;
  v.displayName = saved->displayName.empty() ? saved->login : saved->displayName;
  v.userId = saved->userId;
  v.scopes = saved->tokens.scopes;
  bool adopted = false;
  {
    Lock lock;
    if (s_.epoch == taskEpoch_ && !s_.dropFile && !s_.loggedIn) {
      s_.loggedIn = true;
      s_.access.swap(access);
      s_.userId.swap(userId);
      s_.expiresAtMs = saved->tokens.expiresAtMs;
      s_.view = std::move(v);
      viewChanged();
      adopted = true;
    }
  }
  if (!adopted) return;  // logged out meanwhile: the logout task deletes the file
  have_ = true;
  login_ = std::move(*saved);
  nextValidate_ = monoMs();  // Twitch asks for a validation at start
  evo_log("twitch: auth: logged in as %s (saved login)", login_.login.c_str());
}

void Worker::failLogin(const char* sentence, const char* why) {
  flow_ = false;
  std::string text = sentence;
  bool current;
  {
    Lock lock;
    current = s_.epoch == flowEpoch_ && s_.loginWanted;
    if (current) {
      s_.loginWanted = false;
      s_.view = AuthView();
      s_.view.state = AuthState::Error;
      s_.view.error.swap(text);
      viewChanged();
    }
  }
  if (current) evo_log_warn("twitch: auth: login failed: %s", why);
}

void Worker::startLogin() {
  twitch::DeviceCode code;
  char why[256];
  const char* user = nullptr;
  const Outcome o = callCore(
      [&] {
        twitch::AuthClient auth(evoTwitchHttpVerified(), twitch::kAppClientId);
        code = auth.startDeviceLogin(twitch::kAppScopes);
      },
      why, sizeof why, &user);
  if (o != Outcome::Ok) {
    failLogin(sentenceFor(user, why), why);
    return;
  }

  const unsigned long long now = monoMs();
  intervalMs_ = static_cast<unsigned long long>(code.intervalSec > 0 ? code.intervalSec : 5) * kSecondMs;
  codeEnd_ = now + static_cast<unsigned long long>(code.expiresInSec > 0 ? code.expiresInSec : 600) * kSecondMs;
  nextPoll_ = now + intervalMs_;
  pollFailures_ = 0;
  offlineLogged_ = false;

  AuthView v;
  v.state = AuthState::WaitingForUser;
  v.userCode = code.userCode;
  v.verificationUri = code.verificationUri;
  v.qr = twitch::makeQr(code.verificationUri);
  v.codeExpiresAtMs = static_cast<long long>(codeEnd_);
  const bool noQr = v.qr.size == 0;
  bool current;
  {
    Lock lock;
    current = s_.epoch == flowEpoch_ && s_.loginWanted;
    if (current) {
      s_.view = std::move(v);
      viewChanged();
    }
  }
  if (!current) {
    flow_ = false;
    return;
  }
  code_ = std::move(code);
  evo_log("twitch: auth: login started, the code is good for %d min", code_.expiresInSec / 60);
  if (noQr) evo_log_warn("twitch: auth: no QR code for the login address (%zu bytes)", code_.verificationUri.size());
}

void Worker::poll() {
  if (monoMs() >= codeEnd_) {
    failLogin(kErrExpired, "the code ran out");
    return;
  }
  twitch::PollResult r = twitch::PollResult::Pending;
  twitch::Tokens t;
  char why[256];
  const char* user = nullptr;
  const Outcome o = callCore(
      [&] {
        twitch::AuthClient auth(evoTwitchHttpVerified(), twitch::kAppClientId);
        r = auth.pollDeviceLogin(code_, twitch::kAppScopes, wallMs(), &t);
      },
      why, sizeof why, &user);
  if (!flowCurrent()) {  // cancelled while the poll was out
    flow_ = false;
    if (o == Outcome::Ok && r == twitch::PollResult::Done) {
      discard(t);  // never saved
      revokePending();
    }
    return;
  }
  if (o == Outcome::Offline) {
    // The user may still approve once the connection is back.
    if (!offlineLogged_) evo_log_warn("twitch: auth: no answer while waiting for the user (%s), still waiting", why);
    offlineLogged_ = true;
    nextPoll_ = monoMs() + intervalMs_;
    return;
  }
  if (o != Outcome::Ok) {
    if (++pollFailures_ >= kMaxPollFailures) {
      failLogin(sentenceFor(user, why), why);
      return;
    }
    evo_log_warn("twitch: auth: unexpected answer while waiting for the user (%s)", why);
    nextPoll_ = monoMs() + intervalMs_;
    return;
  }
  pollFailures_ = 0;
  switch (r) {
    case twitch::PollResult::Pending: nextPoll_ = monoMs() + intervalMs_; return;
    case twitch::PollResult::SlowDown:
      intervalMs_ += 5 * kSecondMs;
      nextPoll_ = monoMs() + intervalMs_;
      evo_log("twitch: auth: Twitch asked for slower polling, every %llu ms now", intervalMs_);
      return;
    case twitch::PollResult::Expired: failLogin(kErrExpired, "the code ran out"); return;
    case twitch::PollResult::Denied: failLogin(kErrDenied, "the user turned the login down"); return;
    case twitch::PollResult::Done: finishLogin(t); return;
  }
}

// True while the main thread still wants this device flow.
bool Worker::flowCurrent() noexcept {
  Lock lock;
  return s_.epoch == flowEpoch_ && s_.loginWanted;
}

// Waits ms between tries of finishLogin(). False when the flow was cancelled.
bool Worker::waitFlow(unsigned long long ms) noexcept {
  const unsigned long long until = monoMs() + ms;
  Lock lock;
  for (;;) {
    if (s_.epoch != flowEpoch_ || !s_.loginWanted || s_.logoutWanted) return false;
    const unsigned long long now = monoMs();
    if (now >= until) return true;
    waitOn(&g_work_cv, until - now);
  }
}

// The user approved: find out who they are, then the session starts.
void Worker::finishLogin(twitch::Tokens& t) {
  giveLifetime(t);
  twitch::HelixUser me;
  char why[256] = "";
  const char* user = nullptr;
  Outcome o = Outcome::Failed;
  for (int i = 0; i < kMeTries; ++i) {
    if (i > 0 && !waitFlow(2 * kSecondMs)) break;
    o = callCore(
        [&] {
          twitch::HelixClient helix(evoTwitchHttpVerified(), twitch::kAppClientId, t.access);
          me = helix.me();
        },
        why, sizeof why, &user);
    if (o == Outcome::Ok || o == Outcome::Refused) break;
  }
  if (o == Outcome::Ok && (me.id.empty() || me.login.empty())) {
    o = Outcome::Failed;
    snprintf(why, sizeof why, "Helix /users answered without an id or a login");
  }
  if (o != Outcome::Ok) {
    discard(t);  // never saved
    revokePending();
    failLogin(o == Outcome::Offline ? sentenceFor(user, why) : kErrNoUser, why[0] ? why : "cancelled");
    return;
  }

  twitch::SavedLogin saved;
  saved.tokens = t;
  saved.userId = me.id;
  saved.login = me.login;
  saved.displayName = me.displayName;
  std::string access = t.access;
  std::string userId = me.id;
  AuthView v;
  v.state = AuthState::LoggedIn;
  v.login = me.login;
  v.displayName = me.displayName.empty() ? me.login : me.displayName;
  v.userId = me.id;
  v.scopes = t.scopes;
  if (!flowCurrent()) {  // cancelled while Twitch was being asked
    flow_ = false;
    discard(t);  // never saved
    revokePending();
    return;
  }
  // Saved below when that succeeds; handed out either way (see the file's
  // Durability note).
  have_ = true;
  login_ = std::move(saved);
  nextValidate_ = monoMs() + kValidateMs;
  backoffMs_ = kBackoffMs;
  validateFails_ = 0;
  save();
  bool current;
  {
    Lock lock;
    current = s_.epoch == flowEpoch_ && s_.loginWanted;
    if (current) {
      s_.loginWanted = false;
      s_.loggedIn = true;
      s_.access.swap(access);
      s_.userId.swap(userId);
      s_.expiresAtMs = t.expiresAtMs;
      s_.refreshWanted = false;
      s_.pauseUntil = 0;
      s_.view = std::move(v);
      viewChanged();
      pthread_cond_broadcast(&g_token_cv);
    }
  }
  flow_ = false;
  if (!current) {  // cancelled during the save: the file goes, then the tokens
    dropSession();
    return;
  }
  evo_log("twitch: auth: logged in as %s", login_.login.c_str());
}

void Worker::refresh() {
  twitch::Tokens t;
  char why[256];
  const Outcome o = callCore(
      [&] {
        twitch::AuthClient auth(evoTwitchHttpVerified(), twitch::kAppClientId);
        t = auth.refresh(login_.tokens.refresh, wallMs());
      },
      why, sizeof why);

  if (o == Outcome::Ok) {
    // From here the old refresh token is spent: the new one is all there is.
    giveLifetime(t);
    bool current;
    {
      Lock lock;
      current = s_.epoch == taskEpoch_;
      if (!current) {
        s_.refreshing = false;
        pthread_cond_broadcast(&g_token_cv);
      }
    }
    if (!current) {  // logged out while Twitch was being asked
      discard(t);    // revoked with the old pair, once the file is gone
      dropSession();
      return;
    }
    // Saved below when that succeeds; handed out either way (see the file's
    // Durability note). Callers keep waiting meanwhile (refreshing is still
    // set).
    std::string oldAccess = login_.tokens.access;
    // The scopes go through takeScopes(), which tells the view; swapped, so
    // nothing here can fail with the old refresh token already spent.
    std::vector<std::string> scopes;
    scopes.swap(t.scopes);
    t.scopes.swap(login_.tokens.scopes);
    login_.tokens = std::move(t);
    takeScopes(std::move(scopes));
    save();
    std::string access = login_.tokens.access;
    {
      Lock lock;
      current = s_.epoch == taskEpoch_;
      s_.refreshing = false;
      if (current) {
        s_.access.swap(access);
        s_.expiresAtMs = login_.tokens.expiresAtMs;
        s_.pauseUntil = 0;
      }
      pthread_cond_broadcast(&g_token_cv);
    }
    if (!current) {  // logged out during the save: the old access token goes too
      try {
        pendingRevoke_.push_back(std::move(oldAccess));
      } catch (...) {
      }
      dropSession();
      return;
    }
    backoffMs_ = kBackoffMs;
    evo_log("twitch: auth: refresh ok");
    return;
  }

  if (o == Outcome::Refused) {
    evo_log_warn("twitch: auth: Twitch refused the refresh (%s), logging out", why);
    std::string notice = kRanOut;
    bool current;
    {
      Lock lock;
      current = s_.epoch == taskEpoch_;
      s_.refreshing = false;
      s_.refreshWanted = false;
      if (current) {
        ++s_.epoch;
        s_.loggedIn = false;
        s_.access.clear();
        s_.userId.clear();
        s_.expiresAtMs = 0;
        s_.pauseUntil = 0;
        if (!fileDone_) s_.dropFile = true;
        s_.view = AuthView();
        s_.view.state = AuthState::LoggedOut;
        s_.notice.swap(notice);
        viewChanged();
      }
      pthread_cond_broadcast(&g_token_cv);
    }
    if (!current) return;  // the user logged out meanwhile; that task does the rest
    dropSession();  // the access token may still work: revoked with the rest
    evo_log("twitch: auth: logged out");
    return;
  }

  // No answer: keep the token (it may still have minutes left) and try again
  // by itself after a pause, which also lets auth_token() callers go on
  // without waiting meanwhile.
  {
    Lock lock;
    s_.refreshing = false;
    if (s_.epoch == taskEpoch_) s_.refreshWanted = true;
    s_.pauseUntil = monoMs() + backoffMs_;
    pthread_cond_broadcast(&g_token_cv);
  }
  evo_log_warn("twitch: auth: refresh failed (%s), trying again in %llu s", why, (backoffMs_ + 999) / 1000);
  backoffMs_ = backoffMs_ * 2 > kMaxBackoffMs ? kMaxBackoffMs : backoffMs_ * 2;
}

// The login's scopes as Twitch just listed them; none listed keeps the ones
// known. The view shows them (auth_has_scope()) unless the session moved on.
// Out of memory, the old ones stay: they only decide what the app offers.
void Worker::takeScopes(std::vector<std::string> scopes) noexcept {
  if (scopes.empty() || scopes == login_.tokens.scopes) return;
  try {
    std::string list;
    for (const std::string& scope : scopes) list += (list.empty() ? "" : " ") + scope;
    std::vector<std::string> shown = scopes;
    login_.tokens.scopes.swap(scopes);
    {
      Lock lock;
      if (s_.epoch != taskEpoch_ || s_.view.state != AuthState::LoggedIn) return;
      s_.view.scopes.swap(shown);
      viewChanged();
    }
    evo_log("twitch: auth: the login's scopes are now %s", list.c_str());
  } catch (...) {
    evo_log_error("twitch: auth: out of memory while taking the login's scopes");
  }
}

void Worker::validate() {
  twitch::Identity id;
  char why[256];
  const Outcome o = callCore(
      [&] {
        twitch::AuthClient auth(evoTwitchHttpVerified(), twitch::kAppClientId);
        id = auth.validate(login_.tokens.access);
      },
      why, sizeof why);
  const unsigned long long now = monoMs();

  if (o == Outcome::Ok) {
    validateFails_ = 0;
    nextValidate_ = now + kValidateMs;
    if (id.expiresInSec > 0) {
      const long long expires = wallMs() + static_cast<long long>(id.expiresInSec) * 1000;
      login_.tokens.expiresAtMs = expires;
      Lock lock;
      if (s_.epoch == taskEpoch_) s_.expiresAtMs = expires;
    }
    // Twitch's word on what the token may do; kept in auth.json too.
    if (!id.scopes.empty() && id.scopes != login_.tokens.scopes) {
      takeScopes(std::move(id.scopes));
      save();
    }
    evo_log("twitch: auth: token checked, %d min left", id.expiresInSec / 60);
    return;
  }

  if (o == Outcome::Refused) {
    // Not a logout yet: the refresh decides that.
    evo_log_warn("twitch: auth: Twitch refused the token (%s), refreshing", why);
    nextValidate_ = now + kValidateMs;
    Lock lock;
    if (s_.epoch == taskEpoch_) s_.refreshWanted = true;
    return;
  }

  // No answer (or a strange one): offline is not a logout. The saved token
  // stays in use, and the check comes again sooner.
  ++validateFails_;
  nextValidate_ = now + kRetryMs;
  if (validateFails_ == 1 || validateFails_ % 10 == 0)
    evo_log_warn("twitch: auth: could not check the token (%s), keeping the login, %d time(s) in a row", why,
                 validateFails_);
}

// The worker never ends; it only parks if the core can't run on it.
void* worker(void* arg) noexcept {
  State& s = *static_cast<State*>(arg);
  if (!evoTwitchWorkerCheck()) {
    Lock lock;
    s.dead = true;
    try {
      s.view = AuthView();
      s.view.state = AuthState::Error;
      s.view.error = kNotInBuild;
    } catch (...) {
    }
    viewChanged();
    return nullptr;
  }
  Worker w(s);
  w.run();
  return nullptr;
}

// ---- Main thread ----

// Copies the worker's view when it changed.
void pull(State& s) noexcept {
  const unsigned seq = g_view_seq.load(std::memory_order_acquire);
  if (seq == s.shownSeq) return;
  try {
    AuthView copy;
    {
      Lock lock;
      copy = s.view;
    }
    s.shown = std::move(copy);
    s.shownSeq = seq;
  } catch (...) {
    // Out of memory: the next frame tries again.
  }
}

// No worker: Error, for good.
void goDead(State& s) noexcept {
  {
    Lock lock;
    s.dead = true;
    try {
      s.view = AuthView();
      s.view.state = AuthState::Error;
      s.view.error = kNotInBuild;
    } catch (...) {
    }
    viewChanged();
  }
  pull(s);
}

// Hands the worker auth.json's path once the sandbox is open, looking at most
// every kOpenCheckMs (each look is a system call).
void offerPath(State& s) noexcept {
  const unsigned long long now = monoMs();
  if (now < s.nextOpenCheck) return;
  s.nextOpenCheck = now + kOpenCheckMs;
  if (!sandboxOpen()) return;
  try {
    // evo_data_path() reuses one buffer: each result is copied out at once.
    std::string tmp = evo_data_path(kAuthTmpFile);
    std::string path = evo_data_path(kAuthFile);
    Lock lock;
    s.path.swap(path);
    s.tmpPath.swap(tmp);
    s.pathReady = true;
    if (g_cv_ready) pthread_cond_signal(&g_work_cv);
  } catch (...) {
    return;  // out of memory: the next look tries again
  }
  s.pathSent = true;
}

}  // namespace

void auth_start() noexcept {
  try {
    State& s = state();
    if (s.started) return;
    s.started = true;
    g_main_thread = pthread_self();
    g_main_known.store(true, std::memory_order_release);

    int selftest = evoTwitchSelftestState();
    if (selftest < 0) selftest = evo_twitch_selftest();
    bool ok = selftest == 1;
    if (!ok) {
      evo_log_error("twitch: auth off because the self-test failed");
    } else if (!initCondvars()) {
      evo_log_error("twitch: auth off: no condvar");
      ok = false;
    } else {
      g_cv_ready = true;
      pthread_attr_t attr;
      pthread_attr_init(&attr);
      pthread_attr_setstacksize(&attr, kStackBytes);
      pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
      pthread_t thread;
      const int rc = pthread_create(&thread, &attr, worker, &s);
      pthread_attr_destroy(&attr);
      if (rc != 0) {
        evo_log_error("twitch: auth worker: pthread_create rc=%d", rc);
        ok = false;
      } else {
        evo_log("twitch: auth service up");
      }
    }
    if (!ok) {
      goDead(s);
      return;
    }
    offerPath(s);
    pull(s);
  } catch (...) {
    evo_log_error("twitch: auth service start failed");
  }
}

void auth_frame() noexcept {
  State* s = g_state.load(std::memory_order_relaxed);
  if (!s) return;
  if (s->started && !s->pathSent) offerPath(*s);
  pull(*s);
}

const AuthView& auth_view() noexcept { return state().shown; }

void auth_begin_login() noexcept {
  State& s = state();
  {
    Lock lock;
    if (s.dead || s.loggedIn || s.loginWanted || s.view.state == AuthState::Unknown) return;
    ++s.epoch;
    s.loginWanted = true;
    s.notice.clear();  // an old "ran out" is moot now
    s.view = AuthView();
    s.view.state = AuthState::Starting;
    viewChanged();
    if (g_cv_ready) pthread_cond_signal(&g_work_cv);
  }
  pull(s);
  evo_log("twitch: auth: login asked for");
}

void auth_cancel_login() noexcept {
  State& s = state();
  {
    Lock lock;
    if (s.dead || (!s.loginWanted && s.view.state != AuthState::Error)) return;
    ++s.epoch;
    s.loginWanted = false;
    s.view = AuthView();
    s.view.state = AuthState::LoggedOut;
    viewChanged();
    if (g_cv_ready) pthread_cond_signal(&g_work_cv);
  }
  pull(s);
  evo_log("twitch: auth: login cancelled");
}

void auth_logout() noexcept {
  State& s = state();
  {
    Lock lock;
    if (s.dead) return;
    ++s.epoch;
    s.loginWanted = false;
    s.loggedIn = false;
    s.access.clear();
    s.userId.clear();
    s.expiresAtMs = 0;
    s.refreshWanted = false;
    s.logoutWanted = true;
    s.dropFile = true;
    s.notice.clear();
    s.view = AuthView();
    s.view.state = AuthState::LoggedOut;
    viewChanged();
    if (g_cv_ready) {
      pthread_cond_signal(&g_work_cv);
      pthread_cond_broadcast(&g_token_cv);
    }
  }
  pull(s);
}

bool auth_take_notice(std::string* out) noexcept {
  State* s = g_state.load(std::memory_order_acquire);
  if (!s || !out) return false;
  Lock lock;
  if (s->notice.empty()) return false;
  out->swap(s->notice);
  s->notice.clear();
  return true;
}

bool auth_token(std::string* accessToken, std::string* userId) noexcept {
  State* s = g_state.load(std::memory_order_acquire);
  if (!s) return false;
  // The main thread never waits here: a frame can't stall for a refresh.
  const bool onMain = g_main_known.load(std::memory_order_acquire) && pthread_equal(pthread_self(), g_main_thread);
  if (onMain && !g_main_warned.exchange(true))
    evo_log_error("twitch: auth: auth_token() called on the main thread; it won't wait for a refresh there");
  try {
    Lock lock;
    const unsigned long long deadline = monoMs() + kTokenWaitMs;
    for (;;) {
      if (!s->loggedIn) return false;
      const bool fresh = wallMs() < s->expiresAtMs - kAheadMs;
      if (fresh && !s->refreshing && !s->refreshWanted) break;
      // A refresh just went unanswered: the old token may still do.
      if (!s->refreshing && monoMs() < s->pauseUntil) break;
      if (!s->refreshing && !s->refreshWanted) {
        s->refreshWanted = true;
        pthread_cond_signal(&g_work_cv);
      }
      if (onMain) break;  // what's there
      const unsigned long long now = monoMs();
      if (now >= deadline) break;  // the worker is busy: the old token it is
      waitOn(&g_token_cv, deadline - now);
    }
    if (accessToken) *accessToken = s->access;
    if (userId) *userId = s->userId;
    return true;
  } catch (...) {
    return false;
  }
}

void auth_token_rejected(const std::string& accessToken) noexcept {
  State* s = g_state.load(std::memory_order_acquire);
  if (!s) return;
  {
    Lock lock;
    // Logged out, already replaced, or a refresh already on its way.
    if (!s->loggedIn || accessToken.empty() || s->access != accessToken) return;
    if (s->refreshing || s->refreshWanted) return;
    s->refreshWanted = true;
    pthread_cond_signal(&g_work_cv);
  }
  evo_log("twitch: auth: Twitch refused the token in a request, refreshing");
}

}  // namespace evo_twitch
