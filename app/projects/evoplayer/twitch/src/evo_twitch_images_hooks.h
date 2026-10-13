#pragma once
// Seams for the image service's host test (twitch/tests/images_host_test.cpp),
// which runs evo_twitch_images.cpp on the PC with files in place of the network
// and a clock it moves by hand. The app never calls these.

#include <stddef.h>

namespace evo_twitch {
namespace images_hooks {

// evo_net_http_get_sync's contract without request headers: 0 with *body
// malloc'd (the caller frees it), otherwise a negative EVO_NET_ERR_* and, for
// EVO_NET_ERR_HTTP, the status in *status.
using FetchFn = int (*)(const char* url, char** body, size_t* len, int* status);
// Monotonic milliseconds.
using ClockFn = long long (*)();

// Replaces the network fetch and the clock; nullptr keeps the default. Call
// before the first image() or images_start(): the workers read the fetch hook
// without a lock.
void set(FetchFn fetch, ClockFn clock) noexcept;

struct Stats {
  unsigned long long frame = 0;
  size_t bytes = 0;  // decoded pixels held
  int ready = 0, requested = 0, failed = 0;  // cache entries by state
  int queued = 0;                            // jobs no worker has taken yet
  int finished = 0;                          // jobs done, waiting for images_frame()
  int workers = 0;
  unsigned long long droppedOld = 0;  // queued jobs dropped for a new request
  unsigned long long droppedNew = 0;  // new requests dropped because the queue was full
  unsigned long long evicted = 0;     // pictures freed by the LRU
  unsigned long long overBudget = 0;  // times the cache stayed over budget (logged)
  unsigned long long logLines = 0, logSuppressed = 0;
  unsigned long long decodes = 0;     // full decodes started (header checks passed)
};

// Main thread only.
Stats stats() noexcept;

}  // namespace images_hooks
}  // namespace evo_twitch
