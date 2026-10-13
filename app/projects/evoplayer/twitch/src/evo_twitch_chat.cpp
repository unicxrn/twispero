// Chat service for the channel that is playing. The contract is in
// evo_twitch_chat.h.
//
//   main thread                                       worker (1)
//   chat_frame(channel)
//     the same channel  -> nothing
//     another one       -> wantChannel, ++wantGen --g_mx/g_cv--> leaves the old
//                          log emptied, view reset                 connection, joins
//                                                                  the new channel
//     g_pending > 0     -> takes the inbox  <------inbox (g_mx)--- events, status
//       items of curGen -> log, state, badges,                    lines, states,
//                          emotes                                 badges, emotes,
//       older gens      -> dropped                                 each tagged
//                                                                  with its gen
//
// Generations. Every change of channel (to "" too) bumps wantGen. The worker
// tags what it posts with the gen it works for, and post() drops a batch once
// wantGen has moved on, which is also how the worker learns to leave: it
// posts (or at least checks) after every 250 ms poll. Items of an older gen
// that were already in the inbox are dropped by chat_frame(). So the log only
// ever shows the channel chat_frame() named last.
//
// A connection is one SocketStream, StreamWebSocket and ChatClient on the
// worker's stack, gone (socket closed) before the next attempt starts. The
// badge pictures are fetched once a JOIN has gone through and been handed on,
// so a slow GQL never holds up Live. The third-party emote lists follow, one
// list per poll, once ROOMSTATE has named the channel's user id (see "Third-
// party emotes" below). A connection ends when the channel changes, when
// Twitch sends RECONNECT, on an error, when nothing at all has arrived for
// kSilentMs although we PING every kPingEveryMs (a connection that died
// without a word), or when Twitch hasn't confirmed the JOIN after
// kJoinTimeoutMs. All but a channel change and a RECONNECT after the JOIN
// wait 1, 2, 4 ... 30 s first; a connection that got as far as the JOIN
// starts that over at 1.
//
// File-scope state is trivially constructible, like evo_twitch_data.cpp: the
// C++ objects are placement-constructed into static storage on first use and
// never destroyed.

#include "evo_twitch_chat.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "evo_boot_log.h"
#include "evo_twitch.h"
#include "evo_twitch_data.h"
#include "evo_twitch_internal.h"
#include "evo_twitch_stream.h"
#include "twitch/chat.hpp"
#include "twitch/gql.hpp"
#include "twitch/stream_websocket.hpp"
#include "twitch/third_party_emotes.hpp"

// The host test (twitch/tests/chat_service_host_test.sh) builds with smaller
// limits to see a silent connection dropped, a JOIN go unanswered and a full
// inbox; the app keeps these. It also points the worker at its own server on
// 127.0.0.1: EVO_TWITCH_CHAT_URL_HOOK names a function of the test that
// returns the URL. The app leaves it undefined and talks to Twitch.
#ifdef EVO_TWITCH_CHAT_URL_HOOK
const char* EVO_TWITCH_CHAT_URL_HOOK() noexcept;
#endif
#ifndef EVO_TWITCH_CHAT_INBOX_CAP
#define EVO_TWITCH_CHAT_INBOX_CAP 600
#endif
#ifndef EVO_TWITCH_CHAT_PING_MS
#define EVO_TWITCH_CHAT_PING_MS 60000
#endif
#ifndef EVO_TWITCH_CHAT_SILENT_MS
#define EVO_TWITCH_CHAT_SILENT_MS 150000
#endif
#ifndef EVO_TWITCH_CHAT_JOIN_MS
#define EVO_TWITCH_CHAT_JOIN_MS 20000
#endif

namespace evo_twitch {
namespace {

constexpr size_t kStackBytes = 2 * 1024 * 1024;  // as the data service's worker
constexpr size_t kLogLines = 150;
// Busy channels send 50+ lines a second and the log keeps 150, so a main
// thread that falls behind loses the oldest lines, not memory. A full inbox
// drops a sixth of its events in one go (100 of 600), not one per new line.
constexpr size_t kInboxCap = EVO_TWITCH_CHAT_INBOX_CAP;
constexpr size_t kInboxDrop = kInboxCap / 6 > 0 ? kInboxCap / 6 : 1;
constexpr int kPollMs = 250;                      // also how long a channel change can wait
constexpr unsigned long long kSliceMs = 500;      // the longest single timed wait on g_cv
constexpr int kMaxBackoffSec = 30;
constexpr unsigned long long kPingEveryMs = EVO_TWITCH_CHAT_PING_MS;
constexpr unsigned long long kSilentMs = EVO_TWITCH_CHAT_SILENT_MS;
// Twitch confirms a JOIN within a second or two. One that never comes (a
// channel that doesn't exist or is banned) would otherwise sit on
// "Connecting" for good.
constexpr unsigned long long kJoinTimeoutMs = EVO_TWITCH_CHAT_JOIN_MS;

const char* const kConnecting = "Connecting to chat";
const char* const kConnected = "Connected to chat";
const char* const kReconnecting = "Reconnecting to chat";
const char* const kNotInBuild = "Chat isn't available in this build";

enum ItemKind : int { kEvent, kStatus, kState, kBadges, kEmotes };

// One thing the worker hands to the main thread.
struct Item {
  unsigned gen = 0;  // the wantGen it is for
  int kind = kEvent;
  int state = 0;                // kState: a ChatState
  twitch::ChatEvent event;      // kEvent
  std::string text;             // kStatus: the line
  twitch::BadgeCatalog badges;  // kBadges
  twitch::EmoteMap emotes;      // kEmotes: the channel's, global ones included
};

struct State {
  // Shared with the worker, under g_mx.
  std::string wantChannel;  // the login to follow, "" for none
  unsigned wantGen = 0;     // bumped by every change of wantChannel
  std::vector<Item> inbox;

  // Main thread only.
  std::vector<Item> taken;  // the inbox chat_frame() swapped out; kept for its capacity
  twitch::ChatLog* log = nullptr;  // in g_log_storage
  twitch::BadgeCatalog badges;
  twitch::EmoteMap emotes;
  bool thirdParty = true;  // chat_frame()'s thirdPartyEmotes, last call
  ChatView view;
  std::string asked;  // chat_frame()'s last argument, lower-cased
  unsigned curGen = 0;
  bool started = false;
  bool broken = false;  // no worker: a channel just gets kNotInBuild
};

// The view hands out pointers to the log and the badge list, so building the
// state must not be able to fail.
static_assert(std::is_nothrow_default_constructible<State>::value,
              "State is built on first use and must not throw");
// The log is the one part whose type doesn't promise that: an empty std::deque
// allocates on some standard libraries. On the console's libc++ it owns no
// memory, and ChatLog's constructor only sets its capacity, so building it
// can't fail there. On another library, running out of memory at that point
// would end the program, as it does in any noexcept function.
#if defined(_LIBCPP_VERSION)
static_assert(std::is_nothrow_default_constructible<std::deque<twitch::ChatLine>>::value,
              "an empty deque must not allocate on the console");
#endif

pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;  // wantChannel, wantGen, inbox
// wantGen changed. Set up by chat_start() (initCondvar) before the worker
// exists; until then nobody waits on it, and nothing signals it.
pthread_cond_t g_cv;
clockid_t g_cv_clock = CLOCK_REALTIME;  // the clock of g_cv's timed waits; fixed before the worker starts
bool g_cv_ready = false;                // main thread: g_cv is set up
bool g_sleep_waits = false;             // worker only: timed waits came back early, so waitSignal() sleeps
std::atomic<int> g_pending{0};                     // items in the inbox: chat_frame()'s idle check
std::atomic<bool> g_worker_failed{false};          // the worker's exception check failed
std::atomic<bool> g_third_party{true};             // the setting, for the worker
std::uint32_t g_connects = 0;                      // worker only: feeds the WebSocket seed
alignas(State) unsigned char g_storage[sizeof(State)];
State* g_state = nullptr;  // main thread; the worker gets it as its argument
alignas(twitch::ChatLog) unsigned char g_log_storage[sizeof(twitch::ChatLog)];

// Holds g_mx for one scope, also when an exception leaves it.
class Lock {
 public:
  Lock() noexcept { pthread_mutex_lock(&g_mx); }
  ~Lock() { pthread_mutex_unlock(&g_mx); }
  Lock(const Lock&) = delete;
  Lock& operator=(const Lock&) = delete;
};

// Main thread, before the worker starts: g_cv with its timed waits on
// CLOCK_MONOTONIC, so a change of the wall clock (the console setting its
// time from the network, say) can't stretch a wait. Where that isn't offered
// it falls back to the default clock, CLOCK_REALTIME. False when there is no
// condvar at all.
bool initCondvar() noexcept {
#ifdef EVO_TWITCH_CHAT_TEST_WRONG_CLOCK
  // Host test only: a wall-clock condvar given monotonic deadlines, which lie
  // far in the past, so every timed wait returns at once. That is what a
  // console ignoring the clock attribute would do; waitSignal() must notice.
  g_cv_clock = CLOCK_MONOTONIC;
  return pthread_cond_init(&g_cv, nullptr) == 0;
#endif
  pthread_condattr_t attr;
  if (pthread_condattr_init(&attr) == 0) {
    const bool mono =
        pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) == 0 && pthread_cond_init(&g_cv, &attr) == 0;
    pthread_condattr_destroy(&attr);
    if (mono) {
      g_cv_clock = CLOCK_MONOTONIC;
      return true;
    }
  }
  if (pthread_cond_init(&g_cv, nullptr) != 0) return false;
  g_cv_clock = CLOCK_REALTIME;
  evo_log_warn("twitch: chat: no monotonic condvar, timed waits use the wall clock");
  return true;
}

// Under g_mx: waits on g_cv for up to ms (at most kSliceMs), or until it is
// signalled. Callers measure the time that has passed on data_now_ms() and
// look at the gen again after every call. The slices only matter with the
// wall-clock fallback, where a clock set back mid-wait stretches that slice.
//
// The guard: a console that ignored CLOCK_MONOTONIC on the condvar would read
// the deadline as a wall-clock time long past, every wait would return at
// once, and the worker would spin a core. A timeout that comes back before
// half the time has passed switches this to plain sleeps of up to 100 ms with
// the lock released, so a channel change still gets through within 100 ms.
void waitSignal(unsigned long long ms) noexcept {
  if (ms > kSliceMs) ms = kSliceMs;
  if (g_sleep_waits) {
    const unsigned long long nap = ms < 100 ? ms : 100;
    struct timespec t;
    t.tv_sec = 0;
    t.tv_nsec = static_cast<long>(nap) * 1000000L;
    pthread_mutex_unlock(&g_mx);
    nanosleep(&t, nullptr);
    pthread_mutex_lock(&g_mx);
    return;
  }
  const unsigned long long start = data_now_ms();
  struct timespec until;
  clock_gettime(g_cv_clock, &until);
  until.tv_sec += static_cast<time_t>(ms / 1000);
  until.tv_nsec += static_cast<long>(ms % 1000) * 1000000L;
  if (until.tv_nsec >= 1000000000L) {
    until.tv_sec++;
    until.tv_nsec -= 1000000000L;
  }
  const int rc = pthread_cond_timedwait(&g_cv, &g_mx, &until);
  // Under 20 ms the millisecond clock is too coarse to tell.
  if (rc == ETIMEDOUT && ms >= 20 && data_now_ms() - start < ms / 2) {
    g_sleep_waits = true;
    pthread_mutex_unlock(&g_mx);  // not while the main thread may want the lock
    evo_log_warn("twitch: chat: timed waits return early, sleeping in 100 ms steps instead");
    pthread_mutex_lock(&g_mx);
  }
}

// ---- Worker ----

// Under g_mx: when the inbox is full, drops its oldest kInboxDrop chat events
// in one pass. A Joined (it turns the view Live), status lines, states and
// badge lists stay. Only when there is no event to drop (a main thread that
// hasn't drained for hours) does the oldest item of any kind go instead.
void makeRoom(std::vector<Item>& inbox) {
  if (inbox.size() < kInboxCap) return;
  size_t dropped = 0;
  size_t kept = 0;
  for (size_t i = 0; i < inbox.size(); ++i) {
    Item& item = inbox[i];
    if (dropped < kInboxDrop && item.kind == kEvent && item.event.kind != twitch::ChatEventKind::Joined) {
      ++dropped;
      continue;
    }
    if (kept != i) inbox[kept] = std::move(item);
    ++kept;
  }
  if (dropped == 0) {
    inbox.erase(inbox.begin());
    return;
  }
  inbox.erase(inbox.begin() + static_cast<std::ptrdiff_t>(kept), inbox.end());
}

// Hands items (all for gen) to the main thread in one go, unless the main
// thread has moved on to another channel since: then the items are dropped
// and it returns false, which tells the worker to leave. items ends up empty.
bool post(State& s, unsigned gen, std::vector<Item>& items) noexcept {
  bool current = false;
  bool lost = false;
  {
    Lock lock;
    current = s.wantGen == gen;
    if (current && !items.empty()) {
      try {
        for (Item& item : items) {
          makeRoom(s.inbox);
          s.inbox.push_back(std::move(item));
        }
      } catch (...) {
        lost = true;  // out of memory: the rest of this batch is gone, chat goes on
      }
      g_pending.store(static_cast<int>(s.inbox.size()), std::memory_order_release);
    }
  }
  items.clear();  // outside the lock: freeing a batch of lines takes a moment
  if (lost) evo_log_error("twitch: chat: out of memory, lost some chat lines");
  return current;
}

// A state change plus, unless text is null, a status line. False when the
// main thread has moved on.
bool postState(State& s, unsigned gen, ChatState state, const char* text) noexcept {
  std::vector<Item> items;
  try {
    items.resize(text ? 2 : 1);
    items[0].gen = gen;
    items[0].kind = kState;
    items[0].state = static_cast<int>(state);
    if (text) {
      items[1].gen = gen;
      items[1].kind = kStatus;
      items[1].text = text;
    }
  } catch (...) {
    items.clear();  // out of memory: the view just misses this change
  }
  return post(s, gen, items);
}

// True while the main thread still wants gen.
bool current(State& s, unsigned gen) noexcept {
  Lock lock;
  return s.wantGen == gen;
}

// Waits ms, or less if the main thread moves on. True when it still wants gen.
bool waitCurrent(State& s, unsigned gen, unsigned long long ms) noexcept {
  const unsigned long long until = data_now_ms() + ms;
  Lock lock;
  for (;;) {
    if (s.wantGen != gen) return false;
    const unsigned long long now = data_now_ms();
    if (now >= until) return true;
    waitSignal(until - now);
  }
}

// Waits for a gen newer than `seen` that names a channel, and returns it in
// channel and gen. Gens for "" are only marked seen. Idle, it sleeps until
// chat_frame() signals a change.
void nextChannel(State& s, unsigned& seen, std::string& channel, unsigned& gen) noexcept {
  Lock lock;
  for (;;) {
    if (s.wantGen != seen) {
      if (s.wantChannel.empty()) {
        seen = s.wantGen;
      } else {
        try {
          channel = s.wantChannel;
          seen = gen = s.wantGen;
          return;
        } catch (...) {
          waitSignal(kSliceMs);  // out of memory: try again in a moment
          continue;
        }
      }
    }
    pthread_cond_wait(&g_cv, &g_mx);
  }
}

// The WebSocket's seed (handshake key, frame masks) and the anonymous nick:
// they only have to vary between connections.
std::uint32_t nextSeed() noexcept {
  std::uint64_t x = data_now_ms() ^ (static_cast<std::uint64_t>(++g_connects) << 32);
  x ^= x >> 30;  // splitmix64's finish: nearby inputs give unrelated seeds
  x *= 0xBF58476D1CE4E5B9ull;
  x ^= x >> 27;
  x *= 0x94D049BB133111EBull;
  x ^= x >> 31;
  return static_cast<std::uint32_t>(x);
}

// ---- Third-party emotes ----
//
// Six lists: each service's global one, then each one's for the channel. The
// global ones are kept for the session (Emotes), the channel's for its gen
// (Follow). A round fetches, one list per poll, whichever of the six it
// doesn't have yet, then hands the map of them all to the main thread. A
// round starts once the JOIN has gone through and ROOMSTATE has named the
// channel's user id; if a list failed, one more round starts after the next
// JOIN, and that is all for the channel.

constexpr twitch::EmoteProvider kProviders[] = {twitch::EmoteProvider::SevenTv, twitch::EmoteProvider::Bttv,
                                                twitch::EmoteProvider::Ffz};
constexpr int kProviderCount = 3;
constexpr int kEmoteSteps = 2 * kProviderCount;  // the global lists, then the channel's
constexpr int kEmoteRounds = 2;

const char* providerName(int p) noexcept {
  return p == 0 ? "7TV" : p == 1 ? "BetterTTV" : "FrankerFaceZ";
}

// The global lists, for the session. Worker only.
struct Emotes {
  bool have[kProviderCount] = {};
  std::vector<twitch::ThirdPartyEmote> lists[kProviderCount];
};

// One channel (one gen) across its connections. Worker only.
struct Follow {
  const std::string& channel;
  unsigned gen;
  Emotes& global;
  int delaySec = 1;      // the wait after the next drop
  int badgeTries = 0;    // at most 2: see fetchBadges()
  bool badgesOk = false;
  char lastWhy[256] = "";   // the drop reason logged last (logDrop)
  int repeats = 0;          // drops in a row with that reason
  // Third-party emotes.
  std::string roomId = {};     // from ROOMSTATE; "" until it comes
  int emoteStep = -1;          // the next of kEmoteSteps in this round; -1 between rounds
  int emoteRounds = 0;         // started, at most kEmoteRounds
  bool emotesNeeded = true;    // a list is still missing
  bool emotesAfterJoin = false;  // the last round missed one: the next waits for a JOIN
  bool emoteFailed = false;    // a list failed in this round
  bool have[kProviderCount] = {};
  std::vector<twitch::ThirdPartyEmote> lists[kProviderCount] = {};
};

// Logs why a connection ended: the first time, when the reason changes, and
// every 10th time in a row, so a network that stays down doesn't fill the log
// (at the 30 s cap that is a line every 5 minutes). A JOIN starts it over.
void logDrop(Follow& f, const char* reason, const char* next) noexcept {
  if (f.repeats > 0 && strcmp(reason, f.lastWhy) == 0) {
    if (++f.repeats % 10 == 0)
      evo_log_warn("twitch: chat: %s, %s (%d times in a row)", reason, next, f.repeats);
    return;
  }
  snprintf(f.lastWhy, sizeof f.lastWhy, "%s", reason);
  f.repeats = 1;
  evo_log_warn("twitch: chat: %s, %s", reason, next);
}

// The channel's badge pictures. Chat works without them, so a failure is only
// logged. Fetched right after the first Joined of the channel has been posted
// (Live doesn't wait for GQL), and if that fails, once more after the next
// Joined, which comes with a reconnect.
void fetchBadges(State& s, Follow& f) noexcept {
  ++f.badgeTries;
  std::vector<Item> items;
  char why[256];
  const char* user = evoTwitchRun(
      [&] {
        twitch::GqlClient gql(evoTwitchHttp());
        Item item;
        item.gen = f.gen;
        item.kind = kBadges;
        item.badges = twitch::fetchBadges(gql, f.channel);
        items.push_back(std::move(item));
      },
      why, sizeof why);
  if (user) {
    evo_log_warn("twitch: chat: no badge pictures for %s: %s (%s)%s", f.channel.c_str(), user, why,
                 f.badgeTries < 2 ? ", trying again after the next JOIN" : "");
    return;
  }
  f.badgesOk = true;
  post(s, f.gen, items);
}

// The emote map from every list there is, the channel's last so its names
// win, and 7TV over BetterTTV over FrankerFaceZ.
bool postEmotes(State& s, Follow& f) noexcept {
  std::vector<Item> items;
  size_t count = 0;
  try {
    items.resize(1);
    Item& item = items[0];
    item.gen = f.gen;
    item.kind = kEmotes;
    for (int p = kProviderCount; p-- > 0;)
      if (f.global.have[p]) item.emotes.add(f.global.lists[p]);
    for (int p = kProviderCount; p-- > 0;)
      if (f.have[p]) item.emotes.add(f.lists[p]);
    count = item.emotes.size();
  } catch (...) {
    evo_log_error("twitch: chat: out of memory for the emotes of %s", f.channel.c_str());
    return current(s, f.gen);
  }
  evo_log("twitch: chat: %zu third-party emotes for %s", count, f.channel.c_str());
  return post(s, f.gen, items);
}

// True when a list is due now: a round is running, or one may start (see
// above). Off in the settings, nothing is due; a round already started waits.
bool emotesDue(Follow& f) noexcept {
  if (f.roomId.empty() || !g_third_party.load(std::memory_order_acquire)) return false;
  if (f.emoteStep < 0) {
    if (!f.emotesNeeded || f.emotesAfterJoin || f.emoteRounds >= kEmoteRounds) return false;
    f.emoteStep = 0;
    f.emoteFailed = false;
    ++f.emoteRounds;
  }
  return true;
}

// The round's next list it doesn't have, fetched; after the last, the map.
// Only a failure is logged (with the service, never more than a line a list).
void fetchEmoteStep(State& s, Follow& f) noexcept {
  while (f.emoteStep < kEmoteSteps) {
    const int step = f.emoteStep++;
    const int p = step % kProviderCount;
    const bool global = step < kProviderCount;
    if (global ? f.global.have[p] : f.have[p]) continue;
    std::vector<twitch::ThirdPartyEmote> list;
    char why[256];
    const char* user = evoTwitchRun(
        [&] { list = twitch::fetchThirdPartyEmotes(evoTwitchHttp(), kProviders[p], global ? "" : f.roomId); }, why,
        sizeof why);
    if (user) {
      f.emoteFailed = true;
      evo_log_warn("twitch: chat: no %s %s emotes: %s (%s)%s", providerName(p), global ? "global" : "channel", user,
                   why, f.emoteRounds < kEmoteRounds ? ", trying again after the next JOIN" : "");
    } else if (global) {
      f.global.lists[p].swap(list);
      f.global.have[p] = true;
    } else {
      f.lists[p].swap(list);
      f.have[p] = true;
    }
    break;  // one list per poll
  }
  if (f.emoteStep < kEmoteSteps) return;
  f.emoteStep = -1;
  f.emotesNeeded = f.emoteFailed;
  f.emotesAfterJoin = f.emoteFailed;
  postEmotes(s, f);
}

enum class Ended { Left, Reconnect, Silent, NoJoin };

const char* chatUrl() noexcept {
#ifdef EVO_TWITCH_CHAT_URL_HOOK
  return EVO_TWITCH_CHAT_URL_HOOK();
#else
  return twitch::kChatUrl;
#endif
}

// One connection to the channel's chat. Posts what arrives until the main
// thread moves on (Left), Twitch sends RECONNECT, nothing at all has come for
// kSilentMs (Silent), or the JOIN still isn't confirmed after kJoinTimeoutMs
// (NoJoin). Errors leave by exception. joined is set once the JOIN went
// through.
Ended runConnection(State& s, Follow& f, bool& joined) {
  const std::uint32_t seed = nextSeed();
  SocketStream stream;
  twitch::StreamWebSocket ws(stream, seed);
  twitch::ChatClient chat(ws);
  chat.connectAnonymous(10000u + seed % 90000u, chatUrl());
  // The handshake can take seconds; the channel may have changed meanwhile.
  if (!current(s, f.gen)) {
    ws.close();
    return Ended::Left;
  }
  chat.join(f.channel);
  if (!current(s, f.gen)) {
    ws.close();
    return Ended::Left;
  }

  std::uint64_t frames = chat.framesReceived();
  unsigned long long lastFrame = data_now_ms();
  unsigned long long lastPing = lastFrame;
  const unsigned long long joinSent = lastFrame;
  std::vector<Item> items;
  for (;;) {
    std::vector<twitch::ChatEvent> events = chat.pollEvents(kPollMs);
    bool reconnect = false;
    bool justJoined = false;
    for (twitch::ChatEvent& e : events) {
      if (e.kind == twitch::ChatEventKind::Reconnect) {
        reconnect = true;
        continue;
      }
      if (e.kind == twitch::ChatEventKind::Joined && !joined) {
        joined = justJoined = true;
        f.emotesAfterJoin = false;
        evo_log("twitch: chat: joined %s", f.channel.c_str());
      }
      if (e.kind == twitch::ChatEventKind::RoomState && f.roomId.empty() && !e.room.roomId.empty())
        f.roomId = e.room.roomId;
      Item item;
      item.gen = f.gen;
      item.kind = kEvent;
      item.event = std::move(e);
      items.push_back(std::move(item));
    }
    if (!post(s, f.gen, items)) {  // one lock per poll, however many lines came
      ws.close();
      return Ended::Left;
    }
    if (reconnect) {
      ws.close();
      return Ended::Reconnect;
    }
    // The Joined went to the main thread above, so the view turns Live while
    // this runs instead of waiting for it.
    if (justJoined && !f.badgesOk && f.badgeTries < 2) {
      fetchBadges(s, f);
      lastFrame = lastPing = data_now_ms();  // the fetch's time isn't Twitch's silence
    } else if (joined && emotesDue(f)) {
      fetchEmoteStep(s, f);  // one list, then back to chat
      lastFrame = lastPing = data_now_ms();
    }

    const unsigned long long now = data_now_ms();
    if (!joined && now - joinSent >= kJoinTimeoutMs) {
      ws.close();
      return Ended::NoJoin;
    }
    if (chat.framesReceived() != frames) {
      frames = chat.framesReceived();
      lastFrame = now;
    } else if (now - lastFrame >= kSilentMs) {
      return Ended::Silent;  // no close frame: there is nobody to read it
    }
    if (now - lastPing >= kPingEveryMs) {
      ws.sendText("PING :tmi.twitch.tv");  // Twitch answers with a PONG frame
      lastPing = now;
    }
  }
}

// Chat for one channel (one gen). Returns when the main thread names another.
void follow(State& s, const std::string& channel, unsigned gen, Emotes& global) noexcept {
  Follow f{channel, gen, global};
  if (!postState(s, gen, ChatState::Connecting, kConnecting)) return;

  for (;;) {
    bool joined = false;
    Ended ended = Ended::Left;
    char why[256];
    const char* user = evoTwitchRun([&] { ended = runConnection(s, f, joined); }, why, sizeof why);
    if (!current(s, gen) || (!user && ended == Ended::Left)) return;

    if (joined) {
      f.delaySec = 1;
      f.repeats = 0;
    }
    // A RECONNECT is answered at once only from a connection that got as far
    // as the JOIN, so a server that keeps asking can't make us spin.
    if (!user && ended == Ended::Reconnect && joined) {
      evo_log("twitch: chat: Twitch asked for a reconnect");
      if (!postState(s, gen, ChatState::Connecting, kReconnecting)) return;
      continue;
    }

    char reason[256];
    char next[40];
    snprintf(next, sizeof next, "trying again in %d s", f.delaySec);
    if (user) {
      snprintf(reason, sizeof reason, "%s", why[0] ? why : user);
    } else if (ended == Ended::Reconnect) {
      snprintf(reason, sizeof reason, "Twitch asked for a reconnect before the JOIN went through");
    } else if (ended == Ended::NoJoin) {
      snprintf(reason, sizeof reason, "no answer to the JOIN after %llu s", kJoinTimeoutMs / 1000);
    } else {
      snprintf(reason, sizeof reason, "no traffic for %llu s", kSilentMs / 1000);
    }
    logDrop(f, reason, next);
    char text[200];
    if (!user && ended == Ended::NoJoin)
      snprintf(text, sizeof text, "This chat isn't answering. Trying again in %d s", f.delaySec);
    else if (user && why[0])
      // The technical reason too, so a drop can be diagnosed from the TV
      // without a log (cut short; the log has all of it).
      snprintf(text, sizeof text, "Chat dropped (%.110s). Trying again in %d s", why, f.delaySec);
    else
      snprintf(text, sizeof text, "Chat dropped. Trying again in %d s", f.delaySec);
    if (!postState(s, gen, ChatState::Retrying, text)) return;
    if (!waitCurrent(s, gen, static_cast<unsigned long long>(f.delaySec) * 1000ull)) return;
    f.delaySec = f.delaySec * 2 > kMaxBackoffSec ? kMaxBackoffSec : f.delaySec * 2;
    if (!postState(s, gen, ChatState::Connecting, nullptr)) return;
  }
}

void* worker(void* arg) noexcept {
  State& s = *static_cast<State*>(arg);
  // Before any core call (evo_twitch_internal.h). Without working exceptions
  // this thread can't run the core at all: chat_frame() tells the user, and
  // the thread ends.
  if (!evoTwitchWorkerCheck()) {
    g_worker_failed.store(true, std::memory_order_release);
    return nullptr;
  }
  std::string channel;
  unsigned seen = 0;
  Emotes global;
  for (;;) {
    unsigned gen = 0;
    nextChannel(s, seen, channel, gen);
    follow(s, channel, gen, global);
    evo_log("twitch: chat: left %s", channel.c_str());
  }
  return nullptr;
}

// ---- Main thread ----

// Built on first use, by whichever entry point comes first; never fails (see
// the static_asserts at State).
State& state() noexcept {
  if (!g_state) {
    twitch::ChatLog* log = new (g_log_storage) twitch::ChatLog(kLogLines);
    g_state = new (g_storage) State();
    g_state->log = log;
    g_state->view.log = log;
    g_state->view.badges = &g_state->badges;
    g_state->view.emotes = &g_state->emotes;
  }
  return *g_state;
}

char lowerAscii(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

// raw, lower-cased, equals lower. raw may be null (""). No allocation, so
// chat_frame() costs nothing while the channel stays the same.
bool sameLower(const char* raw, const std::string& lower) noexcept {
  if (!raw) raw = "";
  size_t i = 0;
  for (; raw[i] != '\0'; ++i)
    if (i >= lower.size() || lowerAscii(raw[i]) != lower[i]) return false;
  return i == lower.size();
}

// Twitch's login rules, as twitch::normalizeLogin checks them but without the
// exception: 1 to 25 of a-z, 0-9 and _.
bool isLogin(const std::string& s) noexcept {
  if (s.empty() || s.size() > 25) return false;
  for (const char c : s)
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
  return true;
}

// No worker: chat says so instead of connecting.
void goBroken(State& s) {
  ChatView& v = s.view;
  s.broken = true;
  if (v.channel.empty()) return;
  v.state = ChatState::Off;
  s.log->addStatus(kNotInBuild);
}

// chat_frame() got a different argument.
void switchTo(State& s, const char* raw) {
  ChatView& v = s.view;
  // Everything that allocates comes first, so an exception changes nothing.
  std::string asked = raw ? raw : "";
  for (char& c : asked) c = lowerAscii(c);
  std::string login = isLogin(asked) ? asked : std::string();
  std::string shared = login;

  if (!asked.empty() && login.empty())
    evo_log_warn("twitch: chat: not a Twitch login (%zu bytes), chat stays off", asked.size());
  s.asked.swap(asked);
  if (login == v.channel) return;

  {
    Lock lock;
    s.wantChannel.swap(shared);
    s.curGen = ++s.wantGen;
    if (g_cv_ready) pthread_cond_signal(&g_cv);
  }
  s.log->clear();
  s.badges = twitch::BadgeCatalog();
  s.emotes = twitch::EmoteMap();
  v.channel.swap(login);
  v.state = v.channel.empty() ? ChatState::Off : ChatState::Connecting;
  if (s.broken && !v.channel.empty()) {
    v.state = ChatState::Off;
    s.log->addStatus(kNotInBuild);
  }
}

void apply(State& s, Item& item) {
  ChatView& v = s.view;
  switch (item.kind) {
    case kEvent:
      s.log->apply(item.event, s.thirdParty ? &s.emotes : nullptr);
      if (item.event.kind == twitch::ChatEventKind::Joined) {
        v.state = ChatState::Live;
        s.log->addStatus(kConnected);
      }
      break;
    case kStatus: s.log->addStatus(item.text); break;
    case kState: v.state = static_cast<ChatState>(item.state); break;
    case kBadges: s.badges = std::move(item.badges); break;
    case kEmotes: s.emotes = std::move(item.emotes); break;
    default: break;
  }
}

// Moves what the worker posted into the log and the view.
void drain(State& s) {
  {
    Lock lock;
    s.inbox.swap(s.taken);  // taken is empty here: drain() always leaves it so
    g_pending.store(0, std::memory_order_release);
  }
  int lost = 0;
  for (Item& item : s.taken) {
    if (item.gen != s.curGen) continue;  // for a channel chat_frame() has left
    try {
      apply(s, item);
    } catch (...) {
      ++lost;
    }
  }
  s.taken.clear();
  if (lost) evo_log_error("twitch: chat: could not add %d chat lines", lost);
}

}  // namespace

void chat_start() noexcept {
  try {
    State& s = state();
    if (s.started) return;
    s.started = true;
    // A write to a connection the server already dropped raises SIGPIPE, which
    // would kill the app. OpenSSL's writes can't pass MSG_NOSIGNAL, and the
    // console may not honour SO_NOSIGPIPE, so ignore it: the write then just
    // fails with EPIPE and the chat reconnects.
    signal(SIGPIPE, SIG_IGN);

    int selftest = evoTwitchSelftestState();
    if (selftest < 0) selftest = evo_twitch_selftest();
    bool ok = selftest == 1;
    if (!ok) {
      evo_log_error("twitch: chat off because the self-test failed");
    } else if (!initCondvar()) {
      evo_log_error("twitch: chat off: no condvar");
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
        evo_log_error("twitch: chat worker: pthread_create rc=%d", rc);
        ok = false;
      } else {
        evo_log("twitch: chat service up");
      }
    }
    if (!ok) goBroken(s);
  } catch (...) {
    evo_log_error("twitch: chat service start failed");
  }
}

void chat_frame(const char* channel, bool thirdPartyEmotes) noexcept {
  try {
    State& s = state();
    if (thirdPartyEmotes != s.thirdParty) {
      s.thirdParty = thirdPartyEmotes;
      g_third_party.store(thirdPartyEmotes, std::memory_order_release);
    }
    if (!s.broken && g_worker_failed.load(std::memory_order_acquire)) goBroken(s);
    if (!sameLower(channel, s.asked)) switchTo(s, channel);
    if (g_pending.load(std::memory_order_acquire) > 0) drain(s);
  } catch (...) {
    evo_log_error("twitch: chat: frame failed");
  }
}

const ChatView& chat_view() noexcept { return state().view; }

bool chat_room_explains(const std::string& channel, std::string* out) noexcept {
  try {
    State& s = state();
    if (channel.empty() || !sameLower(channel.c_str(), s.view.channel)) return false;
    std::string why = twitch::roomStateSendReason(s.log->room());
    if (why.empty()) return false;
    out->swap(why);
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace evo_twitch
