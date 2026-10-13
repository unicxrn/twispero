// Proves on hardware that C++ exceptions unwind across translation units
// (the throw happens inside twitch::parseMaster in core/src/hls.cpp) and that
// nlohmann parses floats without iostreams.

#include <atomic>
#include <exception>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "evo_boot_log.h"
#include "evo_twitch.h"
#include "evo_twitch_internal.h"
#include "twitch/error.hpp"
#include "twitch/hls.hpp"
#include "twitch/login.hpp"

namespace {

// 1 passed, 0 failed, -1 not run yet.
std::atomic<int> g_state{-1};
// The same for the exception check on a worker thread (evoTwitchWorkerCheck).
std::atomic<int> g_worker_state{-1};

}  // namespace

// nullptr when exceptions work, otherwise what went wrong.
const char* evoTwitchCheckExceptions() {
  // Thrown in core/src/hls.cpp, caught here by its exact type.
  bool caught = false;
  try {
    twitch::parseMaster("not a playlist");
  } catch (const twitch::ApiError&) {
    caught = true;
  }
  if (!caught) return "parseMaster did not throw ApiError";

  // Caught by a standard-library base class three levels up
  // (NotFoundError -> TwitchError -> std::runtime_error), the way the launcher
  // catches everything as std::exception.
  caught = false;
  try {
    twitch::normalizeLogin("not a login!");
  } catch (const std::runtime_error& e) {
    caught = e.what() != nullptr && e.what()[0] != '\0';
  }
  if (!caught) return "NotFoundError was not caught as std::runtime_error";
  return nullptr;
}

namespace {

// nullptr when the JSON round trip works, otherwise what went wrong.
const char* checkJson() {
  const nlohmann::json j = nlohmann::json::parse("{\"a\":1.5,\"b\":[1,2]}", nullptr, false);
  if (j.is_discarded() || !j.is_object()) return "json: parse failed";
  const auto a = j.find("a");
  if (a == j.end() || !a->is_number_float() || a->get<double>() != 1.5)
    return "json: \"a\" is not the float 1.5";
  const auto b = j.find("b");
  if (b == j.end() || !b->is_array() || b->size() != 2) return "json: \"b\" is not a 2-element array";
  if (j.dump().find("1.5") == std::string::npos) return "json: dump() lost the float";
  return nullptr;
}

}  // namespace

int evoTwitchSelftestState() noexcept { return g_state.load(); }

bool evoTwitchWorkerCheck() noexcept {
  const int known = g_worker_state.load();
  if (known >= 0) return known == 1;
  // Two workers that ask at once both run it; that is harmless.
  int result = 0;
  try {
    if (const char* why = evoTwitchCheckExceptions())
      evo_log_error("twitch: exception check on the worker FAILED: %s", why);
    else
      result = 1;
  } catch (...) {
    evo_log_error("twitch: exception check on the worker FAILED: unexpected exception");
  }
  if (result) evo_log("twitch: exception check on the worker ok");
  g_worker_state = result;
  return result == 1;
}

extern "C" int evo_twitch_selftest(void) noexcept {
  try {
    // If the unwinder is broken the process dies inside the first throw, and
    // this flushed line is what shows where.
    evo_log("twitch: self-test running");
    evo_boot_log_flush();

    const char* why = evoTwitchCheckExceptions();
    if (!why) why = checkJson();
    if (why) {
      evo_log_error("twitch: self-test FAILED: %s", why);
      g_state = 0;
    } else {
      evo_log("twitch: self-test ok");
      g_state = 1;
    }
  } catch (const std::exception& e) {
    evo_log_error("twitch: self-test FAILED: unexpected exception: %s", e.what());
    g_state = 0;
  } catch (...) {
    evo_log_error("twitch: self-test FAILED: unexpected exception");
    g_state = 0;
  }
  evo_boot_log_flush();
  return g_state.load() == 1 ? 1 : 0;
}
