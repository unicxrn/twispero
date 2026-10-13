// evo_twitch_progress.cpp - see evo_twitch_progress.h.
//
// State lives in file-scope static storage, placement-new'd on first use and
// never destroyed (as in evo_twitch_settings.cpp). The writer is a detached
// worker per save: it owns a heap copy of the store and the two paths,
// serializes the copy, writes the .tmp file (flushed to the disk), renames it
// and clears g_writing. The main thread never starts a second one while
// g_writing is set; it marks the save pending and progress_frame() starts it
// once the first is done. progress_save_now() waits for it, then writes on
// the calling thread.

#include "evo_twitch_progress.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "evo_boot_log.h"
#include "evo_data_path.h"
#include "evo_jailbreak.h"
#include "twitch/progress.hpp"
#include "twitch/videos.hpp"  // ageText

namespace evo_twitch {

namespace {

constexpr const char* kProgressFile = "twitch_progress.json";
constexpr const char* kProgressTmpFile = "twitch_progress.json.tmp";
// 200 entries of a few hundred bytes each; anything much bigger isn't ours.
constexpr size_t kMaxProgressBytes = 1024 * 1024;
constexpr int kContinueMax = 20;
constexpr size_t kWriterStack = 256 * 1024;

struct State {
  twitch::ProgressStore store;
  std::vector<VideoProgress> entries;  // store's entries, as the app's type
  std::unordered_map<std::string, size_t> index;  // id -> entries[]
  std::vector<VideoProgress> continueList;
  unsigned revision = 0;
  bool loaded = false;  // the file has been read (or tried) once
  bool dirty = false;   // changed since the last write began
  bool pending = false; // a save waits for the writer
};

alignas(State) unsigned char g_storage[sizeof(State)];
State* g_state = nullptr;
std::atomic<bool> g_writing{false};

State& rawState() noexcept {
  if (!g_state) g_state = new (g_storage) State();
  return *g_state;
}

VideoProgress fromCore(const twitch::WatchProgress& p) {
  VideoProgress v;
  v.id = p.id;
  v.seconds = p.seconds;
  v.duration = p.duration;
  v.updated = p.updated;
  v.title = p.title;
  v.channel = p.channel;
  v.login = p.login;
  v.thumb = p.thumb;
  v.game = p.game;
  v.published = p.published;
  v.type = p.type;
  return v;
}

twitch::WatchProgress toCore(const VideoProgress& v) {
  twitch::WatchProgress p;
  p.id = v.id;
  p.seconds = v.seconds;
  p.duration = v.duration;
  p.updated = v.updated;
  p.title = v.title;
  p.channel = v.channel;
  p.login = v.login;
  p.thumb = v.thumb;
  p.game = v.game;
  p.published = v.published;
  p.type = v.type;
  return p;
}

// The app's copies of the store, after any change to it.
void rebuild(State& s) {
  s.entries.clear();
  s.index.clear();
  for (const twitch::WatchProgress& p : s.store.entries()) {
    s.index[p.id] = s.entries.size();
    s.entries.push_back(fromCore(p));
  }
  s.continueList.clear();
  for (const twitch::WatchProgress& p : s.store.continueWatching(kContinueMax)) s.continueList.push_back(fromCore(p));
  ++s.revision;
}

void readFile(const char* path, std::string& out) {
  out.clear();
  FILE* f = fopen(path, "rb");
  if (!f) return;
  char buf[4096];
  size_t n;
  while (out.size() < kMaxProgressBytes && (n = fread(buf, 1, sizeof buf, f)) > 0) {
    const size_t room = kMaxProgressBytes - out.size();
    out.append(buf, n < room ? n : room);
  }
  fclose(f);
}

// The state, with the file read first if that is due.
State& state() noexcept {
  State& s = rawState();
  if (s.loaded || !evo_jailbreak_is_open()) return s;
  s.loaded = true;
  try {
    std::string json;
    readFile(evo_data_path(kProgressFile), json);
    twitch::ProgressStore fromFile = twitch::parseProgress(json);
    // Anything recorded before the sandbox opened is newer than the file.
    for (auto it = s.store.entries().rbegin(); it != s.store.entries().rend(); ++it) fromFile.record(*it);
    s.store = std::move(fromFile);
    rebuild(s);
    if (!s.store.entries().empty())
      evo_log("twitch: %d videos with saved progress", static_cast<int>(s.store.entries().size()));
  } catch (...) {
    evo_log_warn("twitch: could not read %s", kProgressFile);
  }
  return s;
}

long long unixNow() noexcept {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<long long>(ts.tv_sec);
}

struct WriteJob {
  twitch::ProgressStore store;  // a copy, serialized on the writer
  char tmpPath[256];
  char finalPath[256];
};

// A job for the store as it is now, or nullptr when there is no memory.
// Main thread.
WriteJob* makeJob(const State& s) noexcept {
  WriteJob* job = new (std::nothrow) WriteJob();
  if (!job) return nullptr;
  try {
    job->store = s.store;
  } catch (...) {
    delete job;
    return nullptr;
  }
  // evo_data_path() reuses one buffer: each path is copied out in turn.
  snprintf(job->tmpPath, sizeof job->tmpPath, "%s", evo_data_path(kProgressTmpFile));
  snprintf(job->finalPath, sizeof job->finalPath, "%s", evo_data_path(kProgressFile));
  return job;
}

// Serializes the job's store and writes it to the .tmp file, flushed to the
// disk, then renames that over the old file: a crash or a power cut leaves
// the old file or the new one. True when the new one is in place. Any thread.
bool writeJob(const WriteJob& job) noexcept {
  std::string text;
  try {
    text = twitch::serializeProgress(job.store);
  } catch (...) {
    evo_log_warn("twitch: could not serialize the watch progress");
    return false;
  }
  FILE* f = fopen(job.tmpPath, "wb");
  if (!f) {
    evo_log_warn("twitch: could not open %s to save the watch progress", job.tmpPath);
    return false;
  }
  bool ok = fwrite(text.data(), 1, text.size(), f) == text.size() && fflush(f) == 0;
  // A file system without fsync says EINVAL or ENOTSUP: nothing to do then.
  if (ok && fsync(fileno(f)) != 0 && errno != EINVAL && errno != ENOTSUP)
    evo_log_warn("twitch: fsync of %s failed (errno %d)", job.tmpPath, errno);
  ok = fclose(f) == 0 && ok;
  if (!ok) {
    evo_log_warn("twitch: short write saving the watch progress to %s", job.tmpPath);
    remove(job.tmpPath);
    return false;
  }
  if (rename(job.tmpPath, job.finalPath) == 0) return true;
  // As evo_twitch_settings.cpp: drop the old file and try once more.
  remove(job.finalPath);
  if (rename(job.tmpPath, job.finalPath) == 0) return true;
  evo_log_warn("twitch: could not move %s to %s", job.tmpPath, job.finalPath);
  return false;
}

// The writer thread. Owns the job.
void* writer(void* arg) {
  WriteJob* job = static_cast<WriteJob*>(arg);
  writeJob(*job);
  delete job;
  g_writing = false;
  return nullptr;
}

// Starts the writer with the store as it is now. False when it didn't start.
bool startWrite(State& s) {
  WriteJob* job = makeJob(s);
  if (!job) return false;
  g_writing = true;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, kWriterStack);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_t thread;
  const int rc = pthread_create(&thread, &attr, writer, job);
  pthread_attr_destroy(&attr);
  if (rc != 0) {
    evo_log_warn("twitch: could not start the progress writer (rc=%d)", rc);
    delete job;
    g_writing = false;
    return false;
  }
  s.dirty = false;
  return true;
}

}  // namespace

float progress_fraction(const std::string& id) noexcept {
  try {
    const State& s = state();
    const auto it = s.index.find(id);
    if (it == s.index.end()) return -1.0f;
    const VideoProgress& v = s.entries[it->second];
    if (v.duration <= 0) return -1.0f;
    const float f = static_cast<float>(v.seconds) / static_cast<float>(v.duration);
    return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
  } catch (...) {
    return -1.0f;
  }
}

const VideoProgress* progress_find(const std::string& id) noexcept {
  try {
    const State& s = state();
    const auto it = s.index.find(id);
    return it == s.index.end() ? nullptr : &s.entries[it->second];
  } catch (...) {
    return nullptr;
  }
}

int progress_resume_at(const std::string& id) noexcept {
  try {
    const twitch::WatchProgress* p = state().store.find(id);
    return p ? twitch::resumeAt(*p) : 0;
  } catch (...) {
    return 0;
  }
}

const std::vector<VideoProgress>& progress_continue() noexcept { return state().continueList; }

unsigned progress_revision() noexcept { return state().revision; }

std::string progress_age(const VideoProgress& v) noexcept {
  try {
    return twitch::ageText(v.published, unixNow());
  } catch (...) {
    return std::string();
  }
}

void progress_record(const VideoProgress& e) noexcept {
  try {
    if (e.id.empty()) return;
    State& s = state();
    twitch::WatchProgress p = toCore(e);
    p.updated = unixNow();
    s.store.record(p);
    rebuild(s);
    s.dirty = true;
  } catch (...) {
    evo_log_warn("twitch: could not record the watch progress");
  }
}

void progress_clear() noexcept {
  try {
    State& s = state();
    s.store.clear();
    rebuild(s);
    s.dirty = true;
  } catch (...) {
    evo_log_warn("twitch: could not clear the watch progress");
  }
}

bool progress_save() noexcept {
  try {
    State& s = state();
    if (!s.dirty || !evo_jailbreak_is_open()) return false;
    if (g_writing.load()) {
      s.pending = true;
      return true;
    }
    s.pending = false;
    return startWrite(s);
  } catch (...) {
    return false;
  }
}

bool progress_save_now() noexcept {
  try {
    State& s = state();
    // A write under way finishes first: it may hold an older copy, and two
    // writers must never share the .tmp file.
    for (int i = 0; i < 300 && g_writing.load(); ++i) usleep(10 * 1000);
    if (g_writing.load()) {
      evo_log_warn("twitch: the progress writer is still busy after 3 s, not saving again");
      return false;
    }
    s.pending = false;
    if (!s.dirty || !evo_jailbreak_is_open()) return false;
    WriteJob* job = makeJob(s);
    if (!job) return false;
    s.dirty = false;
    const bool ok = writeJob(*job);
    delete job;
    return ok;
  } catch (...) {
    return false;
  }
}

void progress_frame() noexcept {
  State& s = rawState();
  if (!s.pending || g_writing.load()) return;
  s.pending = false;
  try {
    if (s.dirty) startWrite(s);
  } catch (...) {
  }
}

bool progress_saving() noexcept { return g_writing.load() || (g_state && g_state->pending); }

}  // namespace evo_twitch
