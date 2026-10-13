// Image service for the Twitch screens (thumbnails, box art, avatars). The
// contract is in evo_twitch_images.h.
//
//   main thread                                 workers (3)
//   image(url, maxW, maxH)
//     in the cache       -> the pixels
//     new url + size     -> Job --queue (g_mx)--> fetch (evo_net sync GET)
//                                                 decode (stb_image), shrink
//   images_frame()       <--done list (g_mx)---- the Job, with pixels or an error
//     a few per frame    -> cache entry: Ready, or Failed for 60 s
//     over 24 MiB        -> free the least recently shown, if it has not been
//                           shown for 4 frames
//
// Ownership. A Job is a heap node. The main thread allocates it and links it
// into the queue; a worker unlinks it, fills in the result and links it into
// the done list; the main thread unlinks it there, moves the pixels into the
// cache entry and deletes it. Linking and unlinking cannot throw, so nothing
// under the lock can. The cache (entries) and its counters belong to the main
// thread alone; a worker only ever sees the job it holds.
//
// Pixels are malloc'd and freed with free(). When no shrinking is needed the
// buffer is stb_image's own, which is why STBI_MALLOC/STBI_FREE are pinned to
// malloc/free below.
//
// Every piece of file-scope state is trivially constructible (EVO keeps static
// initialization trivial and never tears objects down at exit). The cache is
// placement-constructed into static storage on first use and never destroyed,
// like the HTTP client in evo_twitch_http.cpp.

#include "evo_twitch_images.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "evo_boot_log.h"
#include "evo_net.h"
#include "evo_twitch_images_hooks.h"

// JPEG (previews, box art) and PNG (some avatars). Everything static, so this
// copy of stb_image cannot clash with the others in the app (cover art, image
// viewer, the kit's icons).
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
// Above kMaxSide on purpose: the header scan then reads the size of a picture
// that is too large, and decode() turns it away by name. Only a side over
// 8192 fails the scan itself (and then reads as "not a JPEG or PNG").
#define STBI_MAX_DIMENSIONS 8192
#define STBI_MALLOC(size) malloc(size)
#define STBI_REALLOC(ptr, size) realloc(ptr, size)
#define STBI_FREE(ptr) free(ptr)
#include "../../stb_image.h"
#pragma GCC diagnostic pop

namespace evo_twitch {
namespace {

constexpr int kWorkers = 3;
constexpr size_t kStackBytes = 2 * 1024 * 1024;  // as the launcher's workers
// Decoded pixels kept in memory. Their textures live in the 64 MiB direct
// memory pool that video uses too, so this stays modest.
constexpr size_t kBudgetBytes = 24u * 1024 * 1024;
// A picture shown this recently is never freed. TextureCache copies the
// buffer into its texture synchronously inside pixels(), so nothing reads it
// after that call; the 4 frames are a safety margin for a screen that holds on
// to a pointer a little longer than it should.
constexpr unsigned long long kKeepFrames = 4;
// A queued job whose picture has not been asked for in this many frames has
// scrolled off screen and may make room for a new one.
constexpr unsigned long long kOffscreenFrames = 30;
constexpr int kQueueCap = 64;
// Finished downloads moved into the cache per frame. Each one the screen draws
// is a texture upload, done synchronously by TextureCache.
constexpr int kPublishPerFrame = 4;
constexpr long long kFailMs = 60 * 1000;           // no retry for this long
constexpr unsigned long long kSweepFrames = 60;    // forget old failures about once a second
constexpr size_t kMaxBodyBytes = 8u * 1024 * 1024;  // a thumbnail is ~30 KB
constexpr int kMaxSide = 4096;                     // checked from the header
constexpr int kLogPerSecond = 3;
constexpr long long kOverBudgetLogMs = 60 * 1000;

// Job::err values besides 0 and the (negative) EVO_NET_ERR_* codes.
constexpr int kErrDecode = 1;
constexpr int kErrTooLarge = 2;
constexpr int kErrMemory = 3;
constexpr int kErrInternal = 4;

struct Job {
  Job* prev = nullptr;
  Job* next = nullptr;
  std::string key;  // the cache key: url, maxW, maxH
  std::string url;
  int maxW = 0, maxH = 0;
  // Filled in by the worker.
  unsigned char* rgba = nullptr;  // owned until moved into the cache
  int w = 0, h = 0;
  int err = 0;                   // 0, an EVO_NET_ERR_* or a kErr*
  int status = 0;                // the HTTP status for EVO_NET_ERR_HTTP
  const char* reason = nullptr;  // stb_image's words for kErrDecode (a literal)

  Job() = default;
  Job(const Job&) = delete;
  Job& operator=(const Job&) = delete;
  ~Job() { free(rgba); }
};

// Oldest first. Under g_mx.
struct JobList {
  Job* head = nullptr;
  Job* tail = nullptr;
  int count = 0;

  void push(Job* j) noexcept {
    j->prev = tail;
    j->next = nullptr;
    if (tail)
      tail->next = j;
    else
      head = j;
    tail = j;
    ++count;
  }
  void unlink(Job* j) noexcept {
    if (j->prev)
      j->prev->next = j->next;
    else
      head = j->next;
    if (j->next)
      j->next->prev = j->prev;
    else
      tail = j->prev;
    j->prev = j->next = nullptr;
    --count;
  }
  Job* pop() noexcept {
    Job* j = head;
    if (j) unlink(j);
    return j;
  }
};

enum class EntryState : unsigned char { Requested, Ready, Failed };

struct Entry {
  EntryState state = EntryState::Requested;
  unsigned char* rgba = nullptr;  // Ready: owned by the cache
  int w = 0, h = 0;
  size_t bytes = 0;
  unsigned long long id = 0;
  unsigned long long touched = 0;  // frame of the last image() call
  long long failedMs = 0;          // Failed: when
};

using EntryMap = std::unordered_map<std::string, Entry>;

struct State {
  // Shared with the workers, under g_mx.
  JobList queue;
  JobList done;

  // Main thread only.
  EntryMap entries;
  std::string key;  // image()'s scratch key, so a lookup does not allocate
  std::vector<std::pair<unsigned long long, EntryMap::iterator>> victims;  // evict()'s scratch
  unsigned long long frame = 1;
  unsigned long long nextId = 0;
  unsigned long long fullFrame = 0;  // the queue was full with nothing to drop in this frame
  unsigned long long nextSweep = 0;
  size_t bytes = 0;  // pixels of the Ready entries
  int failed = 0;    // Failed entries
  int workers = 0;
  bool overBudget = false;  // in an over-budget stretch (logged at its start)
  bool overBudgetLogged = false;
  long long overBudgetLogMs = 0;
  long long logWindowMs = 0;  // failure log rate limit: a window of 1 s
  int logInWindow = 0;
  unsigned long long logHeld = 0;  // failures not logged in the current window

  images_hooks::Stats counters;  // droppedOld, droppedNew, evicted, overBudget, logLines, logSuppressed
};

pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;  // the queue, the done list, construction
pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;    // a job was queued
std::atomic<int> g_done{0};  // jobs in the done list: images_frame()'s lock-free idle check
std::atomic<unsigned long long> g_decodes{0};  // full decodes started (Stats::decodes)
alignas(State) unsigned char g_storage[sizeof(State)];
State* g_state = nullptr;  // written by the main thread; workers get it as their argument
images_hooks::FetchFn g_fetch = nullptr;  // nullptr: evo_net
images_hooks::ClockFn g_clock = nullptr;  // nullptr: CLOCK_MONOTONIC

// Main thread.
State* state() noexcept {
  if (g_state) return g_state;
  pthread_mutex_lock(&g_mx);
  try {
    g_state = new (g_storage) State();
  } catch (...) {
    g_state = nullptr;
  }
  pthread_mutex_unlock(&g_mx);
  return g_state;
}

long long nowMs() noexcept {
  if (g_clock) return g_clock();
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<long long>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// ---- Logging (main thread) ----

// "https://user@static-cdn.jtvnw.net/previews-ttv/x.jpg?t=1" ->
// "static-cdn.jtvnw.net/previews-ttv/x.jpg": no scheme, credentials, query or
// fragment.
void hostPath(const char* url, char* out, size_t cap) noexcept {
  const size_t scheme = strcspn(url, ":/?#");
  const char* p = url;
  if (url[scheme] == ':' && url[scheme + 1] == '/' && url[scheme + 2] == '/') p = url + scheme + 3;
  const size_t authority = strcspn(p, "/?#");
  const char* host = p;
  for (size_t i = 0; i < authority; ++i)
    if (p[i] == '@') host = p + i + 1;
  const char* path = p + authority;
  const size_t pathLen = strcspn(path, "?#");
  snprintf(out, cap, "%.*s%.*s", static_cast<int>(p + authority - host), host,
           static_cast<int>(pathLen), path);
}

// Starts a new one-second window once the old one is over, first owning up to
// the lines it held back.
void rollLogWindow(State& s, long long now) noexcept {
  if (now - s.logWindowMs < 1000) return;
  if (s.logHeld > 0) {
    evo_log_warn("twitch: %llu more image failures not logged", s.logHeld);
    ++s.counters.logLines;
    s.logHeld = 0;
  }
  s.logWindowMs = now;
  s.logInWindow = 0;
}

void logFailure(State& s, const Job& job) noexcept {
  rollLogWindow(s, nowMs());
  if (s.logInWindow >= kLogPerSecond) {
    ++s.logHeld;
    ++s.counters.logSuppressed;
    return;
  }
  ++s.logInWindow;
  ++s.counters.logLines;

  char where[256];
  hostPath(job.url.c_str(), where, sizeof where);
  char why[160];
  switch (job.err) {
    case kErrDecode:
      snprintf(why, sizeof why, "not a JPEG or PNG stb_image can read (%s)",
               job.reason ? job.reason : "no reason given");
      break;
    case kErrTooLarge:
      snprintf(why, sizeof why, "too large (over %d px a side or %zu MiB)", kMaxSide,
               kMaxBodyBytes / (1024 * 1024));
      break;
    case kErrMemory:
      snprintf(why, sizeof why, "out of memory");
      break;
    case kErrInternal:
      snprintf(why, sizeof why, "unexpected error");
      break;
    default:
      // No URL: the sentence then carries no host either; `where` has it.
      evo_net_describe_failure(job.err, job.status, "", why, sizeof why);
      break;
  }
  evo_log_warn("twitch: image %s: %s", where, why);
}

// ---- Decoding (workers) ----

// Fits w x h inside maxW x maxH (0 = no limit), keeping the aspect ratio and
// never growing.
void fitSize(int w, int h, int maxW, int maxH, int* outW, int* outH) noexcept {
  *outW = w;
  *outH = h;
  double scale = 1.0;
  if (maxW > 0 && w > maxW) scale = std::min(scale, static_cast<double>(maxW) / w);
  if (maxH > 0 && h > maxH) scale = std::min(scale, static_cast<double>(maxH) / h);
  if (scale >= 1.0) return;
  int nw = static_cast<int>(std::lround(w * scale));
  int nh = static_cast<int>(std::lround(h * scale));
  if (maxW > 0 && nw > maxW) nw = maxW;
  if (maxH > 0 && nh > maxH) nh = maxH;
  *outW = std::max(1, nw);
  *outH = std::max(1, nh);
}

// Shrinking src samples to dst (dst <= src) along one axis: output i averages
// the source span [i * src / dst, (i + 1) * src / dst), the samples at its ends
// weighted by how much of them it covers. One output's weights add up to 1.
struct Taps {
  std::vector<int> first;   // first source sample of output i
  std::vector<int> count;   // how many
  std::vector<int> offset;  // where its weights start in `weight`
  std::vector<float> weight;
};

void buildTaps(int src, int dst, Taps& t) {
  t.first.resize(static_cast<size_t>(dst));
  t.count.resize(static_cast<size_t>(dst));
  t.offset.resize(static_cast<size_t>(dst));
  t.weight.clear();
  t.weight.reserve(static_cast<size_t>(src) + static_cast<size_t>(dst) * 2);
  const double scale = static_cast<double>(src) / dst;
  for (int i = 0; i < dst; ++i) {
    const double lo = i * scale;
    const double hi = std::min(static_cast<double>(src), (i + 1) * scale);
    int j0 = std::min(static_cast<int>(lo), src - 1);
    int j1 = std::min(static_cast<int>(std::ceil(hi)), src);
    if (j1 <= j0) j1 = j0 + 1;
    t.first[static_cast<size_t>(i)] = j0;
    t.count[static_cast<size_t>(i)] = j1 - j0;
    t.offset[static_cast<size_t>(i)] = static_cast<int>(t.weight.size());
    double sum = 0.0;
    for (int j = j0; j < j1; ++j) {
      const double cover = std::min(hi, j + 1.0) - std::max(lo, static_cast<double>(j));
      const double w = cover > 0.0 ? cover : 0.0;
      t.weight.push_back(static_cast<float>(w));
      sum += w;
    }
    float* ws = t.weight.data() + t.offset[static_cast<size_t>(i)];
    for (int k = 0; k < j1 - j0; ++k)
      ws[k] = sum > 0.0 ? static_cast<float>(ws[k] / sum) : (k == 0 ? 1.0f : 0.0f);
  }
}

unsigned char toByte(float v) noexcept {
  return static_cast<unsigned char>(std::min(255.0f, std::max(0.0f, v + 0.5f)));
}

// Area (box) average of an RGBA image down to tw x th, done on premultiplied
// colour so transparent pixels do not darken the edges they share with opaque
// ones. Returns a malloc'd buffer, or nullptr when out of memory. Throws only
// std::bad_alloc (from the vectors).
unsigned char* shrink(const unsigned char* src, int w, int h, int tw, int th) {
  Taps tx, ty;
  buildTaps(w, tw, tx);
  buildTaps(h, th, ty);
  std::vector<float> row(static_cast<size_t>(w) * 4);  // one output row, summed vertically
  unsigned char* out = static_cast<unsigned char*>(malloc(static_cast<size_t>(tw) * th * 4));
  if (!out) return nullptr;

  for (int oy = 0; oy < th; ++oy) {
    std::fill(row.begin(), row.end(), 0.0f);
    const size_t yo = static_cast<size_t>(oy);
    for (int k = 0; k < ty.count[yo]; ++k) {
      const float wy = ty.weight[static_cast<size_t>(ty.offset[yo] + k)];
      const unsigned char* s = src + static_cast<size_t>(ty.first[yo] + k) * w * 4;
      float* r = row.data();
      for (int x = 0; x < w; ++x, s += 4, r += 4) {
        const float a = s[3] * wy;
        r[0] += s[0] * a;
        r[1] += s[1] * a;
        r[2] += s[2] * a;
        r[3] += a;
      }
    }
    unsigned char* d = out + yo * tw * 4;
    for (int ox = 0; ox < tw; ++ox, d += 4) {
      const size_t xo = static_cast<size_t>(ox);
      float cr = 0.0f, cg = 0.0f, cb = 0.0f, ca = 0.0f;
      for (int k = 0; k < tx.count[xo]; ++k) {
        const float wx = tx.weight[static_cast<size_t>(tx.offset[xo] + k)];
        const float* r = row.data() + static_cast<size_t>(tx.first[xo] + k) * 4;
        cr += r[0] * wx;
        cg += r[1] * wx;
        cb += r[2] * wx;
        ca += r[3] * wx;
      }
      // ca is the average alpha (0..255); cr/cg/cb are colour times alpha.
      if (ca > 1e-3f) {
        d[0] = toByte(cr / ca);
        d[1] = toByte(cg / ca);
        d[2] = toByte(cb / ca);
      } else {
        d[0] = d[1] = d[2] = 0;
      }
      d[3] = toByte(ca);
    }
  }
  return out;
}

void decode(Job& job, const unsigned char* data, size_t len) {
  if (len == 0) {
    job.err = kErrDecode;
    job.reason = "empty reply";
    return;
  }
  if (len > kMaxBodyBytes) {
    job.err = kErrTooLarge;
    return;
  }
  const int ilen = static_cast<int>(len);
  // The header first: a picture that is too large is turned away before any
  // pixel memory is allocated.
  int w = 0, h = 0, n = 0;
  if (!stbi_info_from_memory(data, ilen, &w, &h, &n)) {
    job.err = kErrDecode;
    job.reason = stbi_failure_reason();
    return;
  }
  if (w <= 0 || h <= 0 || w > kMaxSide || h > kMaxSide) {
    job.err = kErrTooLarge;
    return;
  }
  g_decodes.fetch_add(1, std::memory_order_relaxed);
  unsigned char* px = stbi_load_from_memory(data, ilen, &w, &h, &n, 4);
  if (!px) {
    job.err = kErrDecode;
    job.reason = stbi_failure_reason();
    return;
  }
  int tw = w, th = h;
  fitSize(w, h, job.maxW, job.maxH, &tw, &th);
  if (tw != w || th != h) {
    unsigned char* small = nullptr;
    try {
      small = shrink(px, w, h, tw, th);
    } catch (...) {
      small = nullptr;
    }
    stbi_image_free(px);
    if (!small) {
      job.err = kErrMemory;
      return;
    }
    px = small;
  }
  job.rgba = px;
  job.w = tw;
  job.h = th;
}

void runJob(Job& job) noexcept {
  char* body = nullptr;
  size_t len = 0;
  int status = 0;
  try {
    const int rc = g_fetch ? g_fetch(job.url.c_str(), &body, &len, &status)
                           : evo_net_http_get_sync(job.url.c_str(), nullptr, 0, &body, &len, &status);
    if (rc != EVO_NET_OK) {
      job.err = rc < 0 ? rc : kErrInternal;
      job.status = status;
    } else {
      decode(job, reinterpret_cast<const unsigned char*>(body), body ? len : 0);
    }
  } catch (...) {
    free(job.rgba);
    job.rgba = nullptr;
    job.err = kErrInternal;
  }
  free(body);
}

void* worker(void* arg) {
  State* s = static_cast<State*>(arg);
  try {
    for (;;) {
      pthread_mutex_lock(&g_mx);
      while (!s->queue.head) pthread_cond_wait(&g_cv, &g_mx);
      Job* job = s->queue.pop();
      pthread_mutex_unlock(&g_mx);

      runJob(*job);

      pthread_mutex_lock(&g_mx);
      s->done.push(job);
      g_done.store(s->done.count, std::memory_order_release);
      pthread_mutex_unlock(&g_mx);
    }
  } catch (...) {
    evo_log_error("twitch: an image worker stopped on an unexpected exception");
  }
  return nullptr;
}

// ---- Cache (main thread) ----

// Queues a download for s.key. `it` is its entry, or end() when there is none.
void request(State& s, EntryMap::iterator it, const char* url, int maxW, int maxH) {
  if (s.fullFrame == s.frame) return;  // full with nothing to drop: no new scan this frame

  Job* job = new (std::nothrow) Job();
  if (!job) return;
  try {
    job->key = s.key;
    job->url = url;
  } catch (...) {
    delete job;
    return;
  }
  job->maxW = maxW;
  job->maxH = maxH;

  // The entry before the queue: it is the last thing here that can throw.
  if (it == s.entries.end()) {
    try {
      it = s.entries.emplace(s.key, Entry()).first;
    } catch (...) {
      delete job;
      return;
    }
  } else if (it->second.state == EntryState::Failed) {
    --s.failed;
  }
  it->second = Entry();
  it->second.touched = s.frame;

  Job* victim = nullptr;
  bool queued = false;
  pthread_mutex_lock(&g_mx);
  if (s.queue.count >= kQueueCap) {
    // The oldest job for a picture that has left the screen makes room.
    for (Job* j = s.queue.head; j; j = j->next) {
      const EntryMap::const_iterator v = s.entries.find(j->key);
      if (v == s.entries.end() || s.frame - v->second.touched >= kOffscreenFrames) {
        victim = j;
        break;
      }
    }
    if (victim) s.queue.unlink(victim);
  }
  if (s.queue.count < kQueueCap) {
    s.queue.push(job);
    queued = true;
    pthread_cond_signal(&g_cv);
  }
  pthread_mutex_unlock(&g_mx);

  if (victim) {
    s.entries.erase(victim->key);  // never `it`: one job per key
    delete victim;
    ++s.counters.droppedOld;
  }
  if (!queued) {
    // Everything queued is still on screen: this one waits for a later frame.
    s.entries.erase(it);
    delete job;
    s.fullFrame = s.frame;
    ++s.counters.droppedNew;
  }
}

// Moves a finished job's result into its cache entry.
void settle(State& s, Job& job) {
  const EntryMap::iterator it = s.entries.find(job.key);
  // Defensive: a job's entry stays Requested until its job settles here (a
  // queued job that is dropped takes its entry with it).
  if (it == s.entries.end() || it->second.state != EntryState::Requested) return;
  Entry& e = it->second;
  if (job.err == 0 && job.rgba) {
    e.state = EntryState::Ready;
    e.rgba = job.rgba;
    job.rgba = nullptr;
    e.w = job.w;
    e.h = job.h;
    e.bytes = static_cast<size_t>(job.w) * static_cast<size_t>(job.h) * 4;
    e.id = ++s.nextId;
    s.bytes += e.bytes;
  } else {
    e.state = EntryState::Failed;
    e.failedMs = nowMs();
    ++s.failed;
    logFailure(s, job);
  }
}

void publish(State& s) {
  Job* jobs[kPublishPerFrame];
  int n = 0;
  pthread_mutex_lock(&g_mx);
  while (n < kPublishPerFrame && s.done.head) jobs[n++] = s.done.pop();
  g_done.store(s.done.count, std::memory_order_release);
  pthread_mutex_unlock(&g_mx);
  for (int i = 0; i < n; ++i) {
    try {
      settle(s, *jobs[i]);
    } catch (...) {
    }
    delete jobs[i];  // frees the pixels unless settle() took them
  }
}

// Frees the least recently shown pictures until the cache is within budget,
// skipping any shown in the last kKeepFrames frames.
void evict(State& s) {
  s.victims.clear();
  for (EntryMap::iterator it = s.entries.begin(); it != s.entries.end(); ++it) {
    const Entry& e = it->second;
    if (e.state == EntryState::Ready && s.frame - e.touched >= kKeepFrames)
      s.victims.emplace_back(e.touched, it);
  }
  std::sort(s.victims.begin(), s.victims.end(),
            [](const std::pair<unsigned long long, EntryMap::iterator>& a,
               const std::pair<unsigned long long, EntryMap::iterator>& b) { return a.first < b.first; });
  for (const auto& v : s.victims) {
    if (s.bytes <= kBudgetBytes) break;
    Entry& e = v.second->second;
    s.bytes -= e.bytes;
    free(e.rgba);
    s.entries.erase(v.second);
    ++s.counters.evicted;
  }
  s.victims.clear();

  if (s.bytes <= kBudgetBytes) {
    s.overBudget = false;
    return;
  }
  // What is left was all on screen a moment ago. Freeing it would pull pixels
  // from under the screen, so the budget gives way until some of it leaves.
  if (s.overBudget) return;
  s.overBudget = true;
  ++s.counters.overBudget;
  const long long now = nowMs();
  if (!s.overBudgetLogged || now - s.overBudgetLogMs >= kOverBudgetLogMs) {
    s.overBudgetLogged = true;
    s.overBudgetLogMs = now;
    evo_log_warn("twitch: pictures on screen take %zu KiB, over the %zu KiB budget; kept until they leave the screen",
                 s.bytes / 1024, kBudgetBytes / 1024);
  }
}

void sweepFailures(State& s) {
  s.nextSweep = s.frame + kSweepFrames;
  const long long now = nowMs();
  for (EntryMap::iterator it = s.entries.begin(); it != s.entries.end();) {
    if (it->second.state == EntryState::Failed && now - it->second.failedMs >= kFailMs) {
      it = s.entries.erase(it);
      --s.failed;
    } else {
      ++it;
    }
  }
}

}  // namespace

namespace {

// The cache entry for url at that size, or entries.end(); leaves the key in
// s.key for a request.
EntryMap::iterator findEntry(State& s, const char* url, int maxW, int maxH) {
  char size[32];
  snprintf(size, sizeof size, "\x1f%dx%d", maxW, maxH);
  s.key.assign(url);
  s.key.append(size);
  return s.entries.find(s.key);
}

}  // namespace

Image image_if_ready(const char* url, int maxW, int maxH) noexcept {
  Image out;
  try {
    if (!url || !url[0]) return out;
    State* s = state();
    if (!s) return out;
    const EntryMap::iterator it = findEntry(*s, url, maxW < 0 ? 0 : maxW, maxH < 0 ? 0 : maxH);
    if (it == s->entries.end() || it->second.state != EntryState::Ready) return out;
    Entry& e = it->second;
    e.touched = s->frame;
    out.w = e.w;
    out.h = e.h;
    out.rgba = e.rgba;
    out.id = e.id;
  } catch (...) {
  }
  return out;
}

Image image(const char* url, int maxW, int maxH) noexcept {
  Image out;
  try {
    if (!url || !url[0]) return out;
    State* s = state();
    if (!s) return out;
    if (maxW < 0) maxW = 0;
    if (maxH < 0) maxH = 0;

    const EntryMap::iterator it = findEntry(*s, url, maxW, maxH);
    if (it != s->entries.end()) {
      Entry& e = it->second;
      e.touched = s->frame;
      if (e.state == EntryState::Ready) {
        out.w = e.w;
        out.h = e.h;
        out.rgba = e.rgba;
        out.id = e.id;
        return out;
      }
      if (e.state == EntryState::Requested) return out;
      if (nowMs() - e.failedMs < kFailMs) return out;
      // Failed long enough ago: try again.
    }
    request(*s, it, url, maxW, maxH);
  } catch (...) {
  }
  return out;
}

void images_start() noexcept {
  try {
    State* s = state();
    if (!s || s->workers >= kWorkers) return;
    while (s->workers < kWorkers) {
      pthread_attr_t attr;
      pthread_attr_init(&attr);
      pthread_attr_setstacksize(&attr, kStackBytes);
      pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
      pthread_t thread;
      const int rc = pthread_create(&thread, &attr, worker, s);
      pthread_attr_destroy(&attr);
      if (rc != 0) {
        evo_log_error("twitch: image worker: pthread_create rc=%d", rc);
        break;  // the next call tries again
      }
      ++s->workers;
    }
    evo_log("twitch: image service up, %d workers", s->workers);
  } catch (...) {
    evo_log_error("twitch: image service start failed");
  }
}

void images_frame() noexcept {
  State* s = g_state;
  if (!s) return;
  try {
    ++s->frame;
    if (g_done.load(std::memory_order_acquire) > 0) publish(*s);
    if (s->bytes > kBudgetBytes)
      evict(*s);
    else
      s->overBudget = false;
    if (s->failed > 0 && s->frame >= s->nextSweep) sweepFailures(*s);
    if (s->logHeld > 0) rollLogWindow(*s, nowMs());
  } catch (...) {
  }
}

namespace images_hooks {

void set(FetchFn fetch, ClockFn clock) noexcept {
  g_fetch = fetch;
  g_clock = clock;
}

Stats stats() noexcept {
  Stats out;
  State* s = g_state;
  if (!s) return out;
  out = s->counters;
  out.frame = s->frame;
  out.bytes = s->bytes;
  out.workers = s->workers;
  out.decodes = g_decodes.load(std::memory_order_relaxed);
  for (const auto& kv : s->entries) {
    switch (kv.second.state) {
      case EntryState::Requested: ++out.requested; break;
      case EntryState::Ready: ++out.ready; break;
      case EntryState::Failed: ++out.failed; break;
    }
  }
  pthread_mutex_lock(&g_mx);
  out.queued = s->queue.count;
  out.finished = s->done.count;
  pthread_mutex_unlock(&g_mx);
  return out;
}

}  // namespace images_hooks
}  // namespace evo_twitch
