// evo_twitch_history.cpp - see evo_twitch_history.h.
//
// State lives in file-scope static storage, placement-new'd on first use and
// never destroyed (as in evo_twitch_progress.cpp). The writer is a detached
// worker per save: it owns a heap copy of the history and the two paths,
// serializes the copy, writes the .tmp file (flushed to the disk), renames it
// and clears g_writing. The main thread never starts a second one while
// g_writing is set; it marks the save pending and history_frame() starts it
// once the first is done. history_save_now() waits for it, then writes on
// the calling thread.

#include "evo_twitch_history.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "evo_boot_log.h"
#include "evo_data_path.h"
#include "evo_jailbreak.h"
#include "twitch/history.hpp"

namespace evo_twitch {

namespace {

constexpr const char* kHistoryFile = "twitch_history.json";
constexpr const char* kHistoryTmpFile = "twitch_history.json.tmp";
// 30 channels and 8 searches of a few hundred bytes each; anything much
// bigger isn't ours.
constexpr size_t kMaxHistoryBytes = 256 * 1024;
constexpr size_t kWriterStack = 256 * 1024;

struct State {
  twitch::WatchHistory history;
  std::vector<HistoryChannel> channels;  // history's, as the app's type
  std::vector<std::string> searches;
  unsigned revision = 0;
  bool loaded = false;   // the file has been read (or tried) once
  bool dirty = false;    // changed since the last write began
  bool pending = false;  // a save waits for the writer
};

alignas(State) unsigned char g_storage[sizeof(State)];
State* g_state = nullptr;
std::atomic<bool> g_writing{false};

State& rawState() noexcept {
  if (!g_state) g_state = new (g_storage) State();
  return *g_state;
}

// The app's copies of the history, after any change to it.
void rebuild(State& s) {
  s.channels.clear();
  for (const twitch::WatchedChannel& c : s.history.channels()) {
    HistoryChannel h;
    h.login = c.login;
    h.name = c.displayName;
    h.avatarUrl = c.avatarUrl;
    h.watched = c.watched;
    s.channels.push_back(std::move(h));
  }
  s.searches = s.history.searches();
  ++s.revision;
}

void readFile(const char* path, std::string& out) {
  out.clear();
  FILE* f = fopen(path, "rb");
  if (!f) return;
  char buf[4096];
  size_t n;
  while (out.size() < kMaxHistoryBytes && (n = fread(buf, 1, sizeof buf, f)) > 0) {
    const size_t room = kMaxHistoryBytes - out.size();
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
    readFile(evo_data_path(kHistoryFile), json);
    twitch::WatchHistory fromFile = twitch::parseHistory(json);
    // Anything recorded before the sandbox opened is newer than the file.
    const std::vector<twitch::WatchedChannel>& early = s.history.channels();
    for (auto it = early.rbegin(); it != early.rend(); ++it) fromFile.recordChannel(*it);
    const std::vector<std::string>& searches = s.history.searches();
    for (auto it = searches.rbegin(); it != searches.rend(); ++it) fromFile.recordSearch(*it);
    s.history = std::move(fromFile);
    rebuild(s);
    if (!s.history.channels().empty() || !s.history.searches().empty())
      evo_log("twitch: history has %d channels and %d searches", static_cast<int>(s.history.channels().size()),
              static_cast<int>(s.history.searches().size()));
  } catch (...) {
    evo_log_warn("twitch: could not read %s", kHistoryFile);
  }
  return s;
}

long long unixNow() noexcept {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<long long>(ts.tv_sec);
}

struct WriteJob {
  twitch::WatchHistory history;  // a copy, serialized on the writer
  char tmpPath[256];
  char finalPath[256];
};

// A job for the history as it is now, or nullptr when there is no memory.
// Main thread.
WriteJob* makeJob(const State& s) noexcept {
  WriteJob* job = new (std::nothrow) WriteJob();
  if (!job) return nullptr;
  try {
    job->history = s.history;
  } catch (...) {
    delete job;
    return nullptr;
  }
  // evo_data_path() reuses one buffer: each path is copied out in turn.
  snprintf(job->tmpPath, sizeof job->tmpPath, "%s", evo_data_path(kHistoryTmpFile));
  snprintf(job->finalPath, sizeof job->finalPath, "%s", evo_data_path(kHistoryFile));
  return job;
}

// Serializes the job's history and writes it to the .tmp file, flushed to the
// disk, then renames that over the old file: a crash or a power cut leaves
// the old file or the new one. True when the new one is in place. Any thread.
bool writeJob(const WriteJob& job) noexcept {
  std::string text;
  try {
    text = twitch::serializeHistory(job.history);
  } catch (...) {
    evo_log_warn("twitch: could not serialize the watch history");
    return false;
  }
  FILE* f = fopen(job.tmpPath, "wb");
  if (!f) {
    evo_log_warn("twitch: could not open %s to save the watch history", job.tmpPath);
    return false;
  }
  bool ok = fwrite(text.data(), 1, text.size(), f) == text.size() && fflush(f) == 0;
  // A file system without fsync says EINVAL or ENOTSUP: nothing to do then.
  if (ok && fsync(fileno(f)) != 0 && errno != EINVAL && errno != ENOTSUP)
    evo_log_warn("twitch: fsync of %s failed (errno %d)", job.tmpPath, errno);
  ok = fclose(f) == 0 && ok;
  if (!ok) {
    evo_log_warn("twitch: short write saving the watch history to %s", job.tmpPath);
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

// Starts the writer with the history as it is now. False when it didn't start.
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
    evo_log_warn("twitch: could not start the history writer (rc=%d)", rc);
    delete job;
    g_writing = false;
    return false;
  }
  s.dirty = false;
  return true;
}

// A change was made: the app's copies follow, and the file is written (now,
// or once the writer is free; before the sandbox opens it waits for the read).
void changed(State& s) {
  rebuild(s);
  s.dirty = true;
  if (!evo_jailbreak_is_open()) return;
  if (g_writing.load()) {
    s.pending = true;
    return;
  }
  s.pending = false;
  startWrite(s);
}

}  // namespace

const std::vector<HistoryChannel>& history_channels() noexcept { return state().channels; }

const std::vector<std::string>& history_searches() noexcept { return state().searches; }

unsigned history_revision() noexcept { return state().revision; }

void history_record_channel(const std::string& login, const std::string& name,
                            const std::string& avatarUrl) noexcept {
  try {
    State& s = state();
    twitch::WatchedChannel c;
    c.login = login;
    c.displayName = name;
    c.avatarUrl = avatarUrl;
    c.watched = unixNow();
    s.history.recordChannel(c);
    changed(s);
  } catch (...) {
    evo_log_warn("twitch: could not record a channel in the history");
  }
}

void history_record_search(const std::string& query) noexcept {
  try {
    State& s = state();
    s.history.recordSearch(query);
    changed(s);
  } catch (...) {
    evo_log_warn("twitch: could not record a search in the history");
  }
}

void history_remove_search(const std::string& query) noexcept {
  try {
    State& s = state();
    if (s.history.removeSearch(query)) changed(s);
  } catch (...) {
    evo_log_warn("twitch: could not remove a search from the history");
  }
}

void history_clear_channels() noexcept {
  try {
    State& s = state();
    s.history.clearChannels();
    changed(s);
    evo_log("twitch: watch history cleared");
  } catch (...) {
    evo_log_warn("twitch: could not clear the watch history");
  }
}

bool history_save_now() noexcept {
  try {
    State& s = state();
    // A write under way finishes first: it may hold an older copy, and two
    // writers must never share the .tmp file.
    for (int i = 0; i < 300 && g_writing.load(); ++i) usleep(10 * 1000);
    if (g_writing.load()) {
      evo_log_warn("twitch: the history writer is still busy after 3 s, not saving again");
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

void history_frame() noexcept {
  State& s = rawState();
  // A change made before the sandbox opened waits for the file's read (the
  // first ask once it is open) and is written then.
  if (s.dirty && !s.loaded && evo_jailbreak_is_open()) {
    state();
    s.pending = true;
  }
  if (!s.pending || g_writing.load()) return;
  s.pending = false;
  try {
    if (s.dirty) startWrite(s);
  } catch (...) {
  }
}

bool history_saving() noexcept { return g_writing.load() || (g_state && g_state->pending); }

}  // namespace evo_twitch
