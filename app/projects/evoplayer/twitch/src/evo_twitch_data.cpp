// Data service for the Twitch screens. The contract is in evo_twitch_data.h.
//
//   main thread                                   workers (2)
//   request_*(arg)
//     same arg, already on its way -> nothing
//     otherwise                    -> Job --queued[slot] (g_mx)--> GQL query, or
//                                     (replaces a queued one)     Helix with the
//                                                                 user's token for
//                                                                 the follows;
//                                                                 cards built
//   data_frame()                   <--done list (g_mx)----------- the Job
//     arg still the slot's arg?    -> into the slot (Ready, or the error)
//     an older arg                 -> dropped
//
// Two workers, each with its own kinds: one for the anonymous GQL lists, one
// for the follows. A follows job can wait up to 10 s for the login service's
// refresh (auth_token), and nothing else waits behind it.
//
// Paged slots (videos, clips). A first-page job is an ordinary one. A "more"
// job carries the slot's cursor (`after`) and the list it continues (`gen`,
// which goes up whenever a first page lands or the argument changes); it
// lands only on that same list, appended. Both kinds of job share the slot's
// one queue place, so a refresh replaces a next page that is still waiting.
//
// Ownership. A Job is a heap node. The main thread fills in its kind and arg
// and puts it in queued[kind] (one per slot: a newer request replaces the
// waiting one, which the main thread then deletes). A worker takes the
// oldest queued job of its kinds, marks it its `running` one, fills in the
// results without the lock, and links it into the done list. The main thread
// unlinks it there, moves the results into the slot and deletes it. kind and
// arg never change after the job is queued, so the main thread may read them
// under the lock while the worker writes the results.
//
// Every piece of file-scope state is trivially constructible (EVO keeps static
// initialization trivial and never tears objects down at exit). The slots are
// placement-constructed into static storage on first use and never destroyed,
// like the image cache in evo_twitch_images.cpp.

#include "evo_twitch_data.h"

#include <pthread.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <time.h>

#include <atomic>
#include <new>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "evo_boot_log.h"
#include "evo_twitch.h"
#include "evo_twitch_auth.h"
#include "evo_twitch_settings.h"
#include "evo_twitch_internal.h"
#include "twitch/app.hpp"
#include "twitch/browse.hpp"
#include "twitch/error.hpp"
#include "twitch/gql.hpp"
#include "twitch/helix.hpp"
#include "twitch/streams.hpp"
#include "twitch/videos.hpp"

namespace evo_twitch {
namespace {

constexpr size_t kStackBytes = 2 * 1024 * 1024;  // as the launcher's workers
constexpr int kHomeStreamCount = 20;
constexpr int kHomeCategoryCount = 30;
constexpr int kCategoryStreamCount = 30;
constexpr int kSearchCategoryCount = 10;
constexpr int kSearchChannelCount = 10;
constexpr int kFollowedLiveCount = 100;
constexpr int kFollowedChannelCount = 300;
constexpr int kVideoPageCount = 30;   // a page of videos or clips (Twitch's most for one)
constexpr int kVideoListCap = 300;    // the most a paged slot holds: ten pages
constexpr int kCategoryClipCount = 30;
constexpr int kScheduleCount = 3;      // the channel page's next scheduled streams

enum Kind : int {
  kHomeStreams,
  kHomeCategories,
  kCategoryStreams,
  kSearch,
  kChannel,
  kFollowedLive,
  kFollowedChannels,
  kNowPlaying,
  kChannelVideos,
  kChannelClips,
  kCategoryClips,
  kKinds
};
const char* const kKindName[kKinds] = {"home streams",   "home categories",   "category streams", "search",
                                       "channel",        "followed live",     "followed channels",
                                       "now playing",    "channel videos",    "channel clips",
                                       "category clips"};

// A certificate refused for its dates is nearly always the console's clock,
// not Twitch; Following and Home (which both show followed_live()) say so
// instead of the uninformative "No connection".
const char* const kEvoTwitchCheckClock = "Check the PS5's date and time";

struct Job {
  Job* next = nullptr;  // in the done list
  int kind = 0;
  unsigned long long seq = 0;  // request order: a worker takes the oldest first
  std::string arg;             // fixed once queued
  // Filled in by a worker. error: nullptr on success, else the user text.
  // Search: error is the categories' outcome, error2 the channels'.
  std::vector<StreamCard> streams;
  std::vector<CategoryCard> categories;
  std::vector<ChannelCard> channels;
  ChannelPage page;
  std::vector<VideoCard> videos;
  std::vector<ClipCard> clips;
  std::string nextCursor;  // a paged slot's: where the page after this one starts, "" at the end
  const char* error = nullptr;
  const char* error2 = nullptr;
  // A paged slot's next page rather than its first (fixed once queued): the
  // cursor it starts at, never logged, and the list it continues.
  bool more = false;
  std::string after;
  unsigned gen = 0;
};

// Main thread only: what each slot shows or is loading.
struct Meta {
  bool used = false;  // asked for at least once
  std::string arg;    // the argument of the newest request ("" for the home slots)
  // A follows slot whose answer was "the token lacks the scope": asking again
  // can't help until another login, so the same argument asks for nothing
  // (logging out empties the slot, and this with it).
  bool scopeMissing = false;
  // A paged slot: where its next page starts ("" when there is none), and
  // which list it holds (see the top of the file).
  std::string cursor;
  unsigned gen = 0;
};

// The workers: which kinds each one runs.
constexpr int kWorkers = 2;
constexpr unsigned kFollowsKinds = (1u << kFollowedLive) | (1u << kFollowedChannels);
constexpr unsigned kAllKinds = (1u << kKinds) - 1;

struct State {
  // Shared with the workers, under g_mx.
  Job* queued[kKinds] = {};
  Job* running[kWorkers] = {};
  Job* doneHead = nullptr;
  Job* doneTail = nullptr;
  int doneCount = 0;

  // Main thread only.
  Result<std::vector<StreamCard>> homeStreams;
  Result<std::vector<CategoryCard>> homeCategories;
  Result<std::vector<StreamCard>> categoryStreams;
  Result<std::vector<CategoryCard>> searchCategories;
  Result<std::vector<ChannelCard>> searchChannels;
  Result<ChannelPage> channel;
  Result<std::vector<StreamCard>> followedLive;
  Result<std::vector<ChannelCard>> followedChannels;
  Result<ChannelPage> nowPlaying;
  Result<Paged<VideoCard>> channelVideos;
  Result<Paged<ClipCard>> channelClips;
  Result<Paged<ClipCard>> categoryClips;
  Meta meta[kKinds];
  unsigned long long seq = 0;
  bool started = false;
  bool broken = false;  // self-test failed or no worker: every request ends in Error
};

// A slot can be handed out before anything else runs, so building the state
// must not be able to fail.
static_assert(std::is_nothrow_default_constructible<State>::value,
              "State is built on first use and must not throw");

pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;  // queued, running, the done list
pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;    // a job was queued (broadcast: two workers wait)
std::atomic<int> g_done{0};          // jobs in the done list: data_frame()'s idle check
std::atomic<bool> g_worker_ok{true};  // false: a worker's exception check failed
alignas(State) unsigned char g_storage[sizeof(State)];
State* g_state = nullptr;  // written by the main thread; the workers get it through g_workers

// What each worker is handed: written by data_start() before the thread starts.
struct WorkerArg {
  State* state;
  unsigned kinds;  // 1 << Kind for each kind it runs
  int index;       // its place in State::running
};
WorkerArg g_workers[kWorkers];

// Main thread.
State& state() noexcept {
  if (!g_state) g_state = new (g_storage) State();
  return *g_state;
}

unsigned long long monoMs() noexcept {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<unsigned long long>(ts.tv_sec) * 1000ull +
         static_cast<unsigned long long>(ts.tv_nsec / 1000000);
}

// For the thumbnail bucket. Only needs to move on every 5 minutes.
long long realtimeSeconds() noexcept {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<long long>(ts.tv_sec);
}

// ---- Worker ----

StreamCard streamCard(const twitch::StreamInfo& s, long long now) {
  StreamCard c;
  c.login = s.login;
  c.displayName = s.displayName.empty() ? s.login : s.displayName;
  c.title = s.title;
  c.game = s.gameName;
  c.thumbUrl = twitch::liveThumbnailUrl(s.previewUrl, now);
  c.viewers = s.viewers;
  return c;
}

std::vector<StreamCard> streamCards(const std::vector<twitch::StreamInfo>& in) {
  const long long now = realtimeSeconds();
  std::vector<StreamCard> out;
  out.reserve(in.size());
  for (const twitch::StreamInfo& s : in) out.push_back(streamCard(s, now));
  return out;
}

std::vector<CategoryCard> categoryCards(const std::vector<twitch::CategoryInfo>& in) {
  std::vector<CategoryCard> out;
  out.reserve(in.size());
  for (const twitch::CategoryInfo& c : in) {
    CategoryCard card;
    card.name = c.name;
    card.boxArtUrl = c.boxArtUrl;
    card.viewers = c.viewers;
    out.push_back(std::move(card));
  }
  return out;
}

std::vector<ChannelCard> channelCards(const std::vector<twitch::ChannelHit>& in) {
  std::vector<ChannelCard> out;
  out.reserve(in.size());
  for (const twitch::ChannelHit& h : in) {
    ChannelCard card;
    card.login = h.login;
    card.displayName = h.displayName.empty() ? h.login : h.displayName;
    card.avatarUrl = h.profileImageUrl;
    card.liveViewers = h.liveViewers;
    out.push_back(std::move(card));
  }
  return out;
}

bool sameWords(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (tolower(static_cast<unsigned char>(a[i])) != tolower(static_cast<unsigned char>(b[i]))) return false;
  return true;
}

// The channel query has no preview picture; a live channel's is at a fixed
// address (the one topStreams hands out, at the hero's size).
ChannelPage channelPage(const twitch::ChannelInfo& in) {
  ChannelPage p;
  p.login = in.login;
  p.displayName = in.displayName.empty() ? in.login : in.displayName;
  p.description = in.description;
  p.avatarUrl = in.profileImageUrl;
  p.title = in.streamTitle;
  p.game = in.gameName;
  p.followers = in.followers;
  p.viewers = in.viewers;
  p.live = in.live;
  if (in.live && !in.login.empty())
    p.thumbUrl = twitch::liveThumbnailUrl(
        "https://static-cdn.jtvnw.net/previews-ttv/live_user_" + in.login + "-800x450.jpg",
        realtimeSeconds());
  if (in.live) {
    p.startedAt = in.streamStartedAt;
    // The language first, unless a tag says it already (streamers often tag it).
    const std::string language = twitch::languageName(in.language);
    bool tagged = false;
    for (const std::string& t : in.tags) tagged = tagged || sameWords(t, language);
    if (!language.empty() && !tagged) p.tags.push_back(language);
    for (const std::string& t : in.tags) p.tags.push_back(t);
  }
  return p;
}

std::vector<ScheduleItem> scheduleItems(const std::vector<twitch::ScheduleSegment>& in) {
  std::vector<ScheduleItem> out;
  out.reserve(in.size());
  for (const twitch::ScheduleSegment& s : in) {
    ScheduleItem item;
    item.startAt = s.startAt;
    item.title = s.title;
    item.category = s.category;
    out.push_back(std::move(item));
  }
  return out;
}

// A followed live channel as a card; Helix's thumbnail is already sized and
// bucketed like the GQL ones (twitch::HelixClient::followedStreams).
StreamCard followedCard(const twitch::FollowedStream& s) {
  StreamCard c;
  c.login = s.login;
  c.displayName = s.displayName.empty() ? s.login : s.displayName;
  c.title = s.title;
  c.game = s.game;
  c.thumbUrl = s.thumbUrl;
  c.viewers = s.viewers;
  return c;
}

// Helix hands out 300x300 profile pictures; the screens ask for 150x150, the
// size the channel page and Search use, which Twitch serves at the same
// address with the size swapped. Other URLs stay as they are.
std::string avatar150(const std::string& url) {
  constexpr char kFrom[] = "-300x300.";
  const size_t at = url.find(kFrom);
  if (at == std::string::npos) return url;
  std::string out = url;
  out.replace(at, sizeof kFrom - 1, "-150x150.");
  return out;
}

// The follows worker's own copy of the last live list it fetched, so the
// channels job right after a live one (Following asks for both) or within a
// minute of it doesn't fetch it again. Only that worker touches it.
struct FollowedLiveCache {
  std::string userId;
  unsigned long long atMs = 0;
  std::vector<twitch::FollowedStream> live;
};
constexpr unsigned long long kLiveCacheMs = 60000;

// The live followed channels, from the cache when it is the same user's and
// fresh, otherwise from Helix (and then kept).
const std::vector<twitch::FollowedStream>& followedLive(twitch::HelixClient& helix, const std::string& userId,
                                                        FollowedLiveCache& cache) {
  const unsigned long long now = monoMs();
  if (cache.atMs != 0 && cache.userId == userId && now - cache.atMs < kLiveCacheMs) return cache.live;
  cache.atMs = 0;  // nothing valid while a new one is fetched
  cache.live = helix.followedStreams(userId, realtimeSeconds(), kFollowedLiveCount);
  cache.userId = userId;
  cache.atMs = monoMs();
  return cache.live;
}

// Every followed channel with its avatar (GET /users, 100 to a request) and,
// when live, its viewers. Helix's follow list has neither.
std::vector<ChannelCard> followedChannelCards(twitch::HelixClient& helix, const std::string& userId,
                                              FollowedLiveCache& cache) {
  const std::vector<twitch::FollowedChannel> follows = helix.followedChannels(userId, kFollowedChannelCount);
  const std::vector<twitch::FollowedStream>& live = followedLive(helix, userId, cache);
  std::vector<std::string> ids;
  ids.reserve(follows.size());
  for (const twitch::FollowedChannel& f : follows) ids.push_back(f.userId);
  const std::vector<twitch::HelixUser> users = helix.usersById(ids);

  std::unordered_map<std::string, const twitch::HelixUser*> userById;
  for (const twitch::HelixUser& u : users) userById[u.id] = &u;
  std::unordered_map<std::string, int> viewersById;
  for (const twitch::FollowedStream& s : live) viewersById[s.userId] = s.viewers;

  std::vector<ChannelCard> out;
  out.reserve(follows.size());
  for (const twitch::FollowedChannel& f : follows) {
    ChannelCard card;
    card.login = f.login;
    card.displayName = f.displayName.empty() ? f.login : f.displayName;
    const auto user = userById.find(f.userId);
    if (user != userById.end()) card.avatarUrl = avatar150(user->second->profileImageUrl);
    const auto viewers = viewersById.find(f.userId);
    if (viewers != viewersById.end()) card.liveViewers = viewers->second;
    out.push_back(std::move(card));
  }
  return out;
}

// ---- Videos and clips ----

// A paged or windowed slot's argument: its filter's name, ':', then the login
// or category name. The filter goes first, so a name with a ':' in it
// ("Call of Duty: Warzone") still splits at the right place.
const char* const kVideoFilterName[] = {"archive", "highlight", "upload"};
const char* const kClipPeriodName[] = {"day", "week", "month", "all"};

std::string listArg(const char* filter, const std::string& name) { return std::string(filter) + ":" + name; }

// The name after the filter, and the filter's index in names[] (count of
// them); 0 when it isn't one of them, which never happens for an argument
// listArg() made.
std::string splitArg(const std::string& arg, const char* const* names, int count, int* index) {
  const size_t colon = arg.find(':');
  const std::string filter = colon == std::string::npos ? std::string() : arg.substr(0, colon);
  *index = 0;
  for (int i = 0; i < count; ++i)
    if (filter == names[i]) *index = i;
  return colon == std::string::npos ? arg : arg.substr(colon + 1);
}

twitch::VideoType videoType(int index) {
  switch (index) {
    case 1: return twitch::VideoType::Highlight;
    case 2: return twitch::VideoType::Upload;
    default: return twitch::VideoType::Archive;
  }
}

twitch::ClipPeriod clipPeriod(int index) {
  switch (index) {
    case 0: return twitch::ClipPeriod::Day;
    case 2: return twitch::ClipPeriod::Month;
    case 3: return twitch::ClipPeriod::All;
    default: return twitch::ClipPeriod::Week;
  }
}

std::vector<VideoCard> videoCards(const std::vector<twitch::VideoCard>& in) {
  const long long now = realtimeSeconds();
  std::vector<VideoCard> out;
  out.reserve(in.size());
  for (const twitch::VideoCard& v : in) {
    VideoCard c;
    c.id = v.id;
    c.title = v.title;
    c.game = v.gameName;
    /* A broadcast still being recorded has the channel's live preview
     * (fetchChannelVideos); it moves on every 5 minutes, as on a live card.
     * Any other thumbnail comes back as it is. */
    c.thumbUrl = twitch::liveThumbnailUrl(v.thumbnailUrl, now);
    c.age = twitch::ageText(v.publishedAt, now);
    c.published = v.publishedAt;
    c.seconds = v.lengthSeconds;
    c.views = v.viewCount;
    out.push_back(std::move(c));
  }
  return out;
}

// Twitch hands out a clip's picture at 1920x1080; its CDN has the same one at
// any size with the size in the file name swapped (checked live), and the
// cards draw 440x248. Another shape of URL stays as it is.
std::string clipThumb(const std::string& url) {
  constexpr char kFrom[] = "-1920x1080.";
  const size_t at = url.rfind(kFrom);
  if (at == std::string::npos) return url;
  std::string out = url;
  out.replace(at, sizeof kFrom - 1, "-440x248.");
  return out;
}

std::vector<ClipCard> clipCards(const std::vector<twitch::ClipCard>& in) {
  const long long now = realtimeSeconds();
  std::vector<ClipCard> out;
  out.reserve(in.size());
  for (const twitch::ClipCard& v : in) {
    ClipCard c;
    c.slug = v.slug;
    c.title = v.title;
    c.game = v.gameName;
    c.thumbUrl = clipThumb(v.thumbnailUrl);
    c.age = twitch::ageText(v.createdAt, now);
    c.curator = v.curatorName;
    c.channelLogin = v.broadcasterLogin;
    c.channelName = v.broadcasterName.empty() ? v.broadcasterLogin : v.broadcasterName;
    c.seconds = v.durationSeconds;
    c.views = v.viewCount;
    out.push_back(std::move(c));
  }
  return out;
}

// Writes one part's outcome to the log. The argument is a category name, a
// login or a user id: cut short, never a URL or a token. A search query is
// what the user typed, so only its length goes in.
void logPart(const Job& job, const char* part, const char* user, const char* why, size_t count,
             unsigned long long ms) noexcept {
  char what[96];
  if (job.kind == kSearch) {
    size_t chars = 0;  // UTF-8 characters, not bytes
    for (const char c : job.arg)
      if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++chars;
    snprintf(what, sizeof what, "%s%s (%zu characters)", kKindName[job.kind], part, chars);
  } else if (job.arg.empty()) {
    snprintf(what, sizeof what, "%s%s", kKindName[job.kind], part);
  } else {
    snprintf(what, sizeof what, "%s%s \"%.40s\"", kKindName[job.kind], part, job.arg.c_str());
  }
  if (user)
    evo_log_warn("twitch: data: %s failed after %llu ms: %s (%s)", what, ms, user, why);
  else
    evo_log("twitch: data: %s: %zu in %llu ms", what, count, ms);
}

// One part of a job (search has two). Logs the outcome; returns the user text
// on failure, nullptr on success.
template <class Fn>
const char* runPart(const Job& job, const char* part, Fn&& fn) noexcept {
  const unsigned long long t0 = monoMs();
  size_t count = 0;
  char why[256];
  const char* user = evoTwitchRun([&] { count = fn(); }, why, sizeof why);
  logPart(job, part, user, why, count, monoMs() - t0);
  return user;
}

// A follows job: fn(helix, userId) with the user's token, which the login
// service hands out here on the follows worker (it may wait for a refresh).
// job.arg is the user id the screen asked for; a token for anyone else
// (logged out, or another login since) is not used. A 401 for the token is
// reported, and the call is tried once more with the token the refresh
// brings; a 401 for a missing scope is not (a refresh keeps the scopes). The
// same token again on that retry means the refresh got no answer (or the
// 10 s wait ran out): Helix would only refuse it again, so this ends with
// "No connection" instead of calling it. A certificate refused for its dates
// says kEvoTwitchCheckClock instead of "No connection" too.
// Logs the outcome; returns the user text on failure, nullptr on success.
template <class Fn>
const char* runFollowed(const Job& job, Fn&& fn) noexcept {
  std::string refusedToken;  // the token Twitch turned down on the first try
  for (int attempt = 0;; ++attempt) {
    const char* part = attempt == 0 ? "" : " (again)";
    const unsigned long long t0 = monoMs();
    std::string token, userId;
    bool tokenOk = false;
    try {
      tokenOk = auth_token(&token, &userId) && userId == job.arg;
    } catch (...) {
      tokenOk = false;
    }
    if (!tokenOk) {
      logPart(job, part, kEvoTwitchLogInAgain, "not logged in as this user", 0, monoMs() - t0);
      return kEvoTwitchLogInAgain;
    }
    if (attempt > 0 && token == refusedToken) {
      logPart(job, part, kEvoTwitchNoConnection, "no connection (the refresh got no answer)", 0,
              monoMs() - t0);
      return kEvoTwitchNoConnection;
    }
    size_t count = 0;
    enum { kNotRefused, kTokenRefused, kScopeMissing } refused = kNotRefused;
    char why[256];
    char refusal[256] = "";
    const char* user = evoTwitchRun(
        [&] {
          try {
            twitch::HelixClient helix(evoTwitchHttpVerified(), twitch::kAppClientId, token);
            count = fn(helix, userId);
          } catch (const twitch::UnauthorizedError& e) {
            refused = e.missingScope() ? kScopeMissing : kTokenRefused;
            snprintf(refusal, sizeof refusal, "%s", e.what());
          }
        },
        why, sizeof why);
    if (refused != kNotRefused) {
      user = refused == kScopeMissing ? kEvoTwitchLogInFromSettings : kEvoTwitchLogInAgain;
      snprintf(why, sizeof why, "%s", refusal);
    } else if (user == kEvoTwitchNoConnection && strstr(why, "date and time")) {
      user = kEvoTwitchCheckClock;
    }
    logPart(job, part, user, why, count, monoMs() - t0);
    if (refused != kTokenRefused || attempt > 0) return user;
    refusedToken = token;
    auth_token_rejected(token);
  }
}

void runJob(Job& job, FollowedLiveCache& cache) noexcept {
  if (!g_worker_ok.load()) {
    job.error = job.error2 = kEvoTwitchFailed;
    return;
  }
  // GqlClient keeps a reference to the shared client and a copy of the client
  // id; built per job inside the part, so nothing here can throw.
  switch (job.kind) {
    case kHomeStreams:
      job.error = runPart(job, "", [&] {
        twitch::GqlClient gql(evoTwitchHttp());
        job.streams = streamCards(twitch::topStreams(gql, kHomeStreamCount, job.arg));
        return job.streams.size();
      });
      break;
    case kHomeCategories:
      job.error = runPart(job, "", [&] {
        twitch::GqlClient gql(evoTwitchHttp());
        job.categories = categoryCards(twitch::topCategories(gql, kHomeCategoryCount));
        return job.categories.size();
      });
      break;
    case kCategoryStreams:
      job.error = runPart(job, "", [&] {
        twitch::GqlClient gql(evoTwitchHttp());
        std::string name = job.arg;
        std::string lang;
        const std::size_t split = job.arg.find('\n');
        if (split != std::string::npos) {
          name = job.arg.substr(0, split);
          lang = job.arg.substr(split + 1);
        }
        job.streams = streamCards(twitch::categoryStreams(gql, name, kCategoryStreamCount, lang));
        return job.streams.size();
      });
      break;
    case kSearch:
      job.error = runPart(job, " (categories)", [&] {
        twitch::GqlClient gql(evoTwitchHttp());
        job.categories =
            categoryCards(twitch::searchCategories(gql, job.arg, kSearchCategoryCount));
        return job.categories.size();
      });
      job.error2 = runPart(job, " (channels)", [&] {
        twitch::GqlClient gql(evoTwitchHttp());
        job.channels = channelCards(twitch::searchChannels(gql, job.arg, kSearchChannelCount));
        return job.channels.size();
      });
      break;
    case kChannel:
    case kNowPlaying:
      job.error = runPart(job, "", [&] {
        twitch::GqlClient gql(evoTwitchHttp());
        job.page = channelPage(twitch::fetchChannel(gql, job.arg));
        return static_cast<size_t>(1);
      });
      // The channel page shows the next scheduled streams too. A query of its
      // own, so a schedule Twitch won't give (or a change on its side) never
      // costs the page: it is only logged, and the page has none.
      if (job.kind == kChannel && !job.error)
        runPart(job, " (schedule)", [&] {
          twitch::GqlClient gql(evoTwitchHttp());
          job.page.schedule =
              scheduleItems(twitch::fetchSchedule(gql, job.arg, realtimeSeconds(), kScheduleCount));
          return job.page.schedule.size();
        });
      break;
    case kFollowedLive:
      job.error = runFollowed(job, [&](twitch::HelixClient& helix, const std::string& userId) {
        cache.atMs = 0;  // a live list is asked for: always a fresh one
        const std::vector<twitch::FollowedStream>& live = followedLive(helix, userId, cache);
        job.streams.clear();
        job.streams.reserve(live.size());
        for (const twitch::FollowedStream& f : live) job.streams.push_back(followedCard(f));
        return job.streams.size();
      });
      break;
    case kFollowedChannels:
      job.error = runFollowed(job, [&](twitch::HelixClient& helix, const std::string& userId) {
        job.channels = followedChannelCards(helix, userId, cache);
        return job.channels.size();
      });
      break;
    case kChannelVideos:
      job.error = runPart(job, job.more ? " (next page)" : "", [&] {
        int filter = 0;
        const std::string login = splitArg(job.arg, kVideoFilterName, 3, &filter);
        twitch::GqlClient gql(evoTwitchHttp());
        twitch::VideoPage page =
            twitch::fetchChannelVideos(gql, login, videoType(filter), kVideoPageCount, job.after);
        job.videos = videoCards(page.cards);
        job.nextCursor = std::move(page.nextCursor);
        return job.videos.size();
      });
      break;
    case kChannelClips:
      job.error = runPart(job, job.more ? " (next page)" : "", [&] {
        int period = 0;
        const std::string login = splitArg(job.arg, kClipPeriodName, 4, &period);
        twitch::GqlClient gql(evoTwitchHttp());
        twitch::ClipPage page =
            twitch::fetchChannelClips(gql, login, clipPeriod(period), kVideoPageCount, job.after);
        job.clips = clipCards(page.cards);
        job.nextCursor = std::move(page.nextCursor);
        return job.clips.size();
      });
      break;
    case kCategoryClips:
      job.error = runPart(job, "", [&] {
        int period = 0;
        const std::string name = splitArg(job.arg, kClipPeriodName, 4, &period);
        twitch::GqlClient gql(evoTwitchHttp());
        job.clips = clipCards(twitch::fetchCategoryClips(gql, name, clipPeriod(period), kCategoryClipCount));
        return job.clips.size();
      });
      break;
    default:
      job.error = job.error2 = kEvoTwitchFailed;
      break;
  }
}

// Under g_mx: the oldest queued job of the given kinds, taken out of the queue.
Job* takeOldest(State& s, unsigned kinds) noexcept {
  int best = -1;
  for (int k = 0; k < kKinds; ++k)
    if ((kinds & (1u << k)) && s.queued[k] && (best < 0 || s.queued[k]->seq < s.queued[best]->seq)) best = k;
  if (best < 0) return nullptr;
  Job* job = s.queued[best];
  s.queued[best] = nullptr;
  return job;
}

void* worker(void* arg) {
  const WorkerArg* w = static_cast<const WorkerArg*>(arg);
  State* s = w->state;
  // Before any core call (evo_twitch_internal.h).
  if (!evoTwitchWorkerCheck()) g_worker_ok = false;
  FollowedLiveCache cache;  // used by the follows worker only

  for (;;) {
    pthread_mutex_lock(&g_mx);
    Job* job;
    while (!(job = takeOldest(*s, w->kinds))) pthread_cond_wait(&g_cv, &g_mx);
    s->running[w->index] = job;
    pthread_mutex_unlock(&g_mx);

    runJob(*job, cache);

    pthread_mutex_lock(&g_mx);
    s->running[w->index] = nullptr;
    job->next = nullptr;
    if (s->doneTail)
      s->doneTail->next = job;
    else
      s->doneHead = job;
    s->doneTail = job;
    ++s->doneCount;
    g_done.store(s->doneCount, std::memory_order_release);
    pthread_mutex_unlock(&g_mx);
  }
  return nullptr;
}

// ---- Slots (main thread) ----

// A new argument: nothing to show until it lands.
template <class T>
void beginLoad(Result<T>& r) {
  r.status = Status::Loading;
  r.value = T{};
  r.error.clear();
  r.stamp = 0;
}

// The same argument again: a Ready value stays up while it refreshes; after an
// Error it is a retry. The error goes either way, so a refresh that fails
// again sets it afresh.
template <class T>
void beginRefresh(Result<T>& r) {
  r.error.clear();
  if (r.status == Status::Ready) return;
  r.status = Status::Loading;
}

template <class T>
void land(Result<T>& r, T&& value, const char* error, unsigned long long now) {
  if (!error) {
    r.value = std::move(value);
    r.status = Status::Ready;
    r.error.clear();
    r.stamp = now;
  } else if (r.status == Status::Ready) {
    r.error = error;  // a failed refresh: the old value stays
  } else {
    r.status = Status::Error;
    r.error = error;
  }
}

// Calls fn on each Result a slot fills.
template <class Fn>
void forEachResult(State& s, int kind, Fn&& fn) {
  switch (kind) {
    case kHomeStreams: fn(s.homeStreams); break;
    case kHomeCategories: fn(s.homeCategories); break;
    case kCategoryStreams: fn(s.categoryStreams); break;
    case kSearch:
      fn(s.searchCategories);
      fn(s.searchChannels);
      break;
    case kChannel: fn(s.channel); break;
    case kFollowedLive: fn(s.followedLive); break;
    case kFollowedChannels: fn(s.followedChannels); break;
    case kNowPlaying: fn(s.nowPlaying); break;
    case kChannelVideos: fn(s.channelVideos); break;
    case kChannelClips: fn(s.channelClips); break;
    case kCategoryClips: fn(s.categoryClips); break;
    default: break;
  }
}

// A paged slot's answer. A first page replaces the list (as land() does) and
// starts a new one for the next pages; a next page is appended to the list it
// continues, or dropped when that list has gone meanwhile.
template <class T>
void settlePage(Result<Paged<T>>& r, Meta& m, Job& job, std::vector<T>&& cards, unsigned long long now) {
  if (job.more) {
    if (job.gen != m.gen || r.status != Status::Ready) {
      evo_log("twitch: data: dropped a %s next page for an older list", kKindName[job.kind]);
      return;
    }
    if (job.error) {
      r.error = job.error;  // the list stays; asking again retries
      return;
    }
    for (T& c : cards) {
      if (r.value.cards.size() >= static_cast<size_t>(kVideoListCap)) break;
      r.value.cards.push_back(std::move(c));
    }
    m.cursor = job.nextCursor;
    r.value.more = !m.cursor.empty() && r.value.cards.size() < static_cast<size_t>(kVideoListCap);
    return;
  }
  Paged<T> page;
  if (!job.error) {
    m.cursor = job.nextCursor;
    ++m.gen;
    page.cards = std::move(cards);
    if (page.cards.size() > static_cast<size_t>(kVideoListCap)) page.cards.resize(kVideoListCap);
    page.more = !m.cursor.empty() && page.cards.size() < static_cast<size_t>(kVideoListCap);
  }
  land(r, std::move(page), job.error, now);
}

void settle(State& s, Job& job, unsigned long long now) {
  Meta& m = s.meta[job.kind];
  if (!m.used || job.arg != m.arg) {
    evo_log("twitch: data: dropped a %s answer for an older request", kKindName[job.kind]);
    return;
  }
  if (job.error == kEvoTwitchLogInFromSettings) m.scopeMissing = true;
  switch (job.kind) {
    case kHomeStreams: land(s.homeStreams, std::move(job.streams), job.error, now); break;
    case kHomeCategories: land(s.homeCategories, std::move(job.categories), job.error, now); break;
    case kCategoryStreams: land(s.categoryStreams, std::move(job.streams), job.error, now); break;
    case kSearch:
      land(s.searchCategories, std::move(job.categories), job.error, now);
      land(s.searchChannels, std::move(job.channels), job.error2, now);
      break;
    case kChannel: land(s.channel, std::move(job.page), job.error, now); break;
    case kFollowedLive: land(s.followedLive, std::move(job.streams), job.error, now); break;
    case kFollowedChannels: land(s.followedChannels, std::move(job.channels), job.error, now); break;
    case kNowPlaying: land(s.nowPlaying, std::move(job.page), job.error, now); break;
    case kChannelVideos: settlePage(s.channelVideos, m, job, std::move(job.videos), now); break;
    case kChannelClips: settlePage(s.channelClips, m, job, std::move(job.clips), now); break;
    case kCategoryClips: settlePage(s.categoryClips, m, job, std::move(job.clips), now); break;
    default: break;
  }
}

// Under g_mx: a job for kind + arg is waiting, running or finished but not
// yet settled, so its answer is on the way. A paged slot's next pages count
// only with countMore: a refresh or a new argument wants a first page.
bool onTheWay(const State& s, int kind, const std::string& arg, bool countQueued,
              bool countMore = false) noexcept {
  auto counts = [&](const Job* j) { return j->kind == kind && j->arg == arg && (countMore || !j->more); };
  if (countQueued && s.queued[kind] && counts(s.queued[kind])) return true;
  for (const Job* r : s.running)
    if (r && counts(r)) return true;
  for (const Job* j = s.doneHead; j; j = j->next)
    if (counts(j)) return true;
  return false;
}

// Ends a request at once with "Something went wrong" (no worker to run it).
void failNow(State& s, int kind) {
  forEachResult(s, kind, [](auto& r) {
    using T = typename std::decay<decltype(r.value)>::type;
    land(r, T{}, kEvoTwitchFailed, 0);
  });
}

// Puts a job for kind + arg in the queue, replacing the one waiting there.
void enqueue(State& s, int kind, const std::string& arg) {
  if (s.broken) {
    failNow(s, kind);
    return;
  }
  Job* job = new Job();
  try {
    job->arg = arg;
  } catch (...) {
    delete job;
    throw;
  }
  job->kind = kind;
  job->seq = ++s.seq;
  pthread_mutex_lock(&g_mx);
  Job* old = s.queued[kind];
  s.queued[kind] = job;
  pthread_cond_broadcast(&g_cv);  // only one of the two workers takes this kind
  pthread_mutex_unlock(&g_mx);
  delete old;
}

void request(int kind, const std::string& arg) noexcept {
  try {
    State& s = state();
    Meta& m = s.meta[kind];
    if (m.used && m.arg == arg) {
      // The token lacks the scope: the Error stays until another login.
      if (m.scopeMissing) return;
      pthread_mutex_lock(&g_mx);
      const bool coming = onTheWay(s, kind, arg, true);
      pthread_mutex_unlock(&g_mx);
      if (coming) return;  // loading, or already refreshing
      forEachResult(s, kind, [](auto& r) { beginRefresh(r); });
      enqueue(s, kind, arg);
      return;
    }

    // A new argument (or the first request): the slot empties, a waiting job
    // for the old argument goes, and one that is already running is dropped
    // when it lands.
    m.arg = arg;
    m.used = true;
    m.cursor.clear();
    ++m.gen;
    forEachResult(s, kind, [](auto& r) { beginLoad(r); });
    pthread_mutex_lock(&g_mx);
    Job* stale = s.queued[kind];
    s.queued[kind] = nullptr;
    const bool coming = onTheWay(s, kind, arg, false);  // A -> B -> A while A runs
    pthread_mutex_unlock(&g_mx);
    delete stale;
    if (!coming) enqueue(s, kind, arg);
  } catch (...) {
    evo_log_error("twitch: data: %s request failed", kKindName[kind]);
  }
}

// A follows slot goes back to Idle: nothing asked for, a waiting job gone, and
// an answer on its way dropped when it lands (settle() checks `used`).
void forget(State& s, int kind) noexcept {
  try {
    s.meta[kind] = Meta();
    forEachResult(s, kind, [](auto& r) { r = typename std::decay<decltype(r)>::type{}; });
    pthread_mutex_lock(&g_mx);
    Job* stale = s.queued[kind];
    s.queued[kind] = nullptr;
    pthread_mutex_unlock(&g_mx);
    delete stale;
  } catch (...) {
    evo_log_error("twitch: data: could not empty the %s", kKindName[kind]);
  }
}

// The follows belong to the user they were asked for: once nobody is logged
// in, or someone else is, their slots empty.
void forgetOtherUsersFollows(State& s) noexcept {
  const int kinds[] = {kFollowedLive, kFollowedChannels};
  if (!s.meta[kFollowedLive].used && !s.meta[kFollowedChannels].used) return;
  const AuthView& v = auth_view();
  const bool in = v.state == AuthState::LoggedIn;
  for (const int kind : kinds) {
    const Meta& m = s.meta[kind];
    if (m.used && (!in || m.arg != v.userId)) {
      forget(s, kind);
      evo_log("twitch: data: %s emptied: not logged in as their user any more", kKindName[kind]);
    }
  }
}

template <class T>
bool wantsMore(const Result<Paged<T>>& r) {
  return r.status == Status::Ready && r.value.more;
}

// A paged slot's next page (evo_twitch_data.h): only for a Ready list with
// more to come and nothing for the slot on its way.
void requestMore(int kind) noexcept {
  try {
    State& s = state();
    Meta& m = s.meta[kind];
    if (!m.used || m.cursor.empty() || s.broken) return;
    const bool wanted = kind == kChannelVideos ? wantsMore(s.channelVideos) : wantsMore(s.channelClips);
    if (!wanted) return;
    pthread_mutex_lock(&g_mx);
    const bool coming = onTheWay(s, kind, m.arg, true, true);
    pthread_mutex_unlock(&g_mx);
    if (coming) return;

    Job* job = new Job();
    try {
      job->arg = m.arg;
      job->after = m.cursor;
    } catch (...) {
      delete job;
      throw;
    }
    job->kind = kind;
    job->more = true;
    job->gen = m.gen;
    job->seq = ++s.seq;
    forEachResult(s, kind, [](auto& r) { r.error.clear(); });  // each failure shows as a change
    pthread_mutex_lock(&g_mx);
    Job* old = s.queued[kind];  // none: nothing was on its way
    s.queued[kind] = job;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mx);
    delete old;
  } catch (...) {
    evo_log_error("twitch: data: %s next page request failed", kKindName[kind]);
  }
}

// A next page of the list the slot holds is on its way.
bool loadingMore(int kind) noexcept {
  State& s = state();
  const Meta& m = s.meta[kind];
  if (!m.used) return false;
  pthread_mutex_lock(&g_mx);
  bool found = false;
  auto mine = [&](const Job* j) { return j && j->kind == kind && j->more && j->gen == m.gen && j->arg == m.arg; };
  if (mine(s.queued[kind])) found = true;
  for (const Job* r : s.running)
    if (mine(r)) found = true;
  for (const Job* j = s.doneHead; j && !found; j = j->next)
    if (mine(j)) found = true;
  pthread_mutex_unlock(&g_mx);
  return found;
}

void requestFollowed(int kind, const std::string& userId) noexcept {
  if (userId.empty()) {
    State& s = state();
    if (s.meta[kind].used) forget(s, kind);
    return;
  }
  request(kind, userId);
}

// Starts a worker on w. False when the thread could not be made.
bool spawnWorker(WorkerArg* w) noexcept {
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, kStackBytes);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_t thread;
  const int rc = pthread_create(&thread, &attr, worker, w);
  pthread_attr_destroy(&attr);
  if (rc != 0) evo_log_error("twitch: data worker %d: pthread_create rc=%d", w->index, rc);
  return rc == 0;
}

}  // namespace

unsigned long long data_now_ms() noexcept { return monoMs(); }

std::string uptime_text(const std::string& startedAt) noexcept {
  try {
    return twitch::uptimeText(startedAt, realtimeSeconds());
  } catch (...) {
    return std::string();
  }
}

std::string starts_in_text(const std::string& startAt) noexcept {
  try {
    return twitch::startsInText(startAt, realtimeSeconds());
  } catch (...) {
    return std::string();
  }
}

const Result<std::vector<StreamCard>>& home_streams() noexcept { return state().homeStreams; }
const Result<std::vector<CategoryCard>>& home_categories() noexcept { return state().homeCategories; }
const Result<std::vector<StreamCard>>& category_streams() noexcept { return state().categoryStreams; }
const Result<std::vector<CategoryCard>>& search_categories() noexcept { return state().searchCategories; }
const Result<std::vector<ChannelCard>>& search_channels() noexcept { return state().searchChannels; }
const Result<ChannelPage>& channel_page() noexcept { return state().channel; }
const Result<std::vector<StreamCard>>& followed_live() noexcept { return state().followedLive; }
const Result<std::vector<ChannelCard>>& followed_channels() noexcept { return state().followedChannels; }
const Result<ChannelPage>& now_playing() noexcept { return state().nowPlaying; }

void request_home_streams() noexcept {
  try {
    request(kHomeStreams, settings().browseLanguage);
  } catch (...) {
    evo_log_error("twitch: data: home streams request failed");
  }
}
void request_home_categories() noexcept { request(kHomeCategories, std::string()); }
void request_category_streams(const std::string& name) noexcept {
  try {
    const std::string& lang = settings().browseLanguage;
    request(kCategoryStreams, lang.empty() ? name : name + "\n" + lang);
  } catch (...) {
    evo_log_error("twitch: data: category streams request failed");
  }
}
void request_channel(const std::string& login) noexcept { request(kChannel, login); }
void request_now_playing(const std::string& login) noexcept { request(kNowPlaying, login); }
void request_followed_live(const std::string& userId) noexcept { requestFollowed(kFollowedLive, userId); }
void request_followed_channels(const std::string& userId) noexcept {
  requestFollowed(kFollowedChannels, userId);
}
const Result<Paged<VideoCard>>& channel_videos() noexcept { return state().channelVideos; }
const Result<Paged<ClipCard>>& channel_clips() noexcept { return state().channelClips; }
const Result<Paged<ClipCard>>& category_clips() noexcept { return state().categoryClips; }

void request_channel_videos(const std::string& login, VideoFilter filter) noexcept {
  try {
    request(kChannelVideos, listArg(kVideoFilterName[static_cast<int>(filter)], login));
  } catch (...) {
    evo_log_error("twitch: data: channel videos request failed");
  }
}
void request_channel_clips(const std::string& login, ClipPeriod period) noexcept {
  try {
    request(kChannelClips, listArg(kClipPeriodName[static_cast<int>(period)], login));
  } catch (...) {
    evo_log_error("twitch: data: channel clips request failed");
  }
}
void request_category_clips(const std::string& name, ClipPeriod period) noexcept {
  try {
    request(kCategoryClips, listArg(kClipPeriodName[static_cast<int>(period)], name));
  } catch (...) {
    evo_log_error("twitch: data: category clips request failed");
  }
}
void request_more_channel_videos() noexcept { requestMore(kChannelVideos); }
void request_more_channel_clips() noexcept { requestMore(kChannelClips); }
bool channel_videos_loading_more() noexcept { return loadingMore(kChannelVideos); }
bool channel_clips_loading_more() noexcept { return loadingMore(kChannelClips); }

bool followed_scope_missing() noexcept {
  const State& s = state();
  return s.meta[kFollowedLive].scopeMissing || s.meta[kFollowedChannels].scopeMissing;
}

void request_search(const std::string& query) noexcept {
  bool blank = true;
  for (const char c : query)
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') blank = false;
  if (!blank) {
    request(kSearch, query);
    return;
  }
  // Nothing to search for: no results, no request. A search still running
  // for an earlier query is dropped when it lands.
  try {
    State& s = state();
    Meta& m = s.meta[kSearch];
    m.arg = query;
    m.used = true;
    const unsigned long long now = monoMs();
    forEachResult(s, kSearch, [now](auto& r) {
      using T = typename std::decay<decltype(r.value)>::type;
      beginLoad(r);
      land(r, T{}, nullptr, now);
    });
    pthread_mutex_lock(&g_mx);
    Job* stale = s.queued[kSearch];
    s.queued[kSearch] = nullptr;
    pthread_mutex_unlock(&g_mx);
    delete stale;
  } catch (...) {
    evo_log_error("twitch: data: search request failed");
  }
}

void data_start() noexcept {
  try {
    State& s = state();
    if (s.started) return;
    s.started = true;

    int selftest = evoTwitchSelftestState();
    if (selftest < 0) selftest = evo_twitch_selftest();
    bool ok = selftest == 1;
    if (!ok) {
      evo_log_error("twitch: data service off because the self-test failed");
    } else {
      // The follows worker first: when it can't start, the other one runs
      // the follows too.
      g_workers[1] = WorkerArg{&s, kFollowsKinds, 1};
      const bool follows = spawnWorker(&g_workers[1]);
      g_workers[0] = WorkerArg{&s, follows ? kAllKinds & ~kFollowsKinds : kAllKinds, 0};
      ok = spawnWorker(&g_workers[0]);
      if (ok) evo_log("twitch: data service up (%s)", follows ? "two workers" : "one worker");
    }
    if (ok) return;

    // No worker: what is already queued ends in Error now, and so does every
    // later request (enqueue() checks `broken`).
    s.broken = true;
    for (int k = 0; k < kKinds; ++k) {
      pthread_mutex_lock(&g_mx);
      Job* stale = s.queued[k];
      s.queued[k] = nullptr;
      pthread_mutex_unlock(&g_mx);
      if (stale) failNow(s, k);
      delete stale;
    }
  } catch (...) {
    evo_log_error("twitch: data service start failed");
  }
}

void data_frame() noexcept {
  State* s = g_state;
  if (!s) return;
  forgetOtherUsersFollows(*s);
  if (g_done.load(std::memory_order_acquire) == 0) return;
  pthread_mutex_lock(&g_mx);
  Job* jobs = s->doneHead;
  s->doneHead = s->doneTail = nullptr;
  s->doneCount = 0;
  g_done.store(0, std::memory_order_release);
  pthread_mutex_unlock(&g_mx);

  const unsigned long long now = monoMs();
  while (jobs) {
    Job* next = jobs->next;
    try {
      settle(*s, *jobs, now);
    } catch (...) {
      evo_log_error("twitch: data: could not store a %s answer", kKindName[jobs->kind]);
    }
    delete jobs;
    jobs = next;
  }
}

}  // namespace evo_twitch
