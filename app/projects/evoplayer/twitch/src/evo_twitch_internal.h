#pragma once
// Shared between the Twitch module's own files (twitch/src). Not part of the
// C ABI in evo_twitch.h.

#include <stddef.h>
#include <stdio.h>

#include <exception>

#include "twitch/error.hpp"
#include "twitch/http.hpp"

// The HTTP client on EVO's network code (evo_twitch_http.cpp). Created on the
// first call and never destroyed. Safe to call and use from any thread.
twitch::HttpClient& evoTwitchHttp();

// The same over verified TLS only (the CA bundle built into the app, the host
// name checked), and no header follows a redirect to another host. Every call
// that carries a user token goes through this one. A certificate that fails,
// a wrong console clock included, is a NetworkError saying why.
twitch::HttpClient& evoTwitchHttpVerified();

// The launcher has nothing in flight: no lookup, no start and no play request
// waiting (evo_twitch_launcher_starting() leaves a quality switch's lookup
// out). Main thread only. For launcher_host_test.cpp.
bool evoTwitchLauncherIdle() noexcept;

// What evo_twitch_selftest() found: 1 passed, 0 failed, -1 not run yet.
int evoTwitchSelftestState() noexcept;

// The self-test's exception check on its own, for another thread to repeat.
// nullptr when it passes, otherwise the reason. Call it inside a try block:
// a half-working unwinder can let an unexpected exception through.
const char* evoTwitchCheckExceptions();

// Unwinding on a worker thread is not the same path as on the main thread
// (its own stack, its own TLS), so a worker proves it before its first core
// call. The first worker to ask runs evoTwitchCheckExceptions() and logs the
// outcome; later calls return what it found. True when it passed. Any thread.
bool evoTwitchWorkerCheck() noexcept;

// What the screens and toasts say when a Twitch request fails.
inline constexpr const char* kEvoTwitchNoConnection = "No connection";
inline constexpr const char* kEvoTwitchOffline = "This channel is offline";
inline constexpr const char* kEvoTwitchNotFound = "Not found";
inline constexpr const char* kEvoTwitchBadAnswer = "Twitch didn't answer as expected";
inline constexpr const char* kEvoTwitchFailed = "Something went wrong";
// Only for a video (twitch::SubscriberOnlyError).
inline constexpr const char* kEvoTwitchSubscribersOnly = "This video is for subscribers only";
// Only for calls with the user's token (the follows).
inline constexpr const char* kEvoTwitchLogInAgain = "Log in again";
inline constexpr const char* kEvoTwitchLogInFromSettings = "Log in again from Settings";

// Calls fn(). When it throws, returns one of the sentences above for the user
// and writes the exception's own message to why[cap] for the log; returns
// nullptr when fn() returned normally (why is then ""). Typed catch clauses,
// most specific first (HttpStatusError is an ApiError), the same mechanism
// the boot self-test proves on hardware. cap must be at least 1.
template <class Fn>
const char* evoTwitchRun(Fn&& fn, char* why, size_t cap) noexcept {
  why[0] = '\0';
  try {
    fn();
    return nullptr;
  } catch (const twitch::NetworkError& e) {
    snprintf(why, cap, "%s", e.what());
    return kEvoTwitchNoConnection;
  } catch (const twitch::OfflineError& e) {
    snprintf(why, cap, "%s", e.what());
    return kEvoTwitchOffline;
  } catch (const twitch::NotFoundError& e) {
    snprintf(why, cap, "%s", e.what());
    return kEvoTwitchNotFound;
  } catch (const twitch::SubscriberOnlyError& e) {
    snprintf(why, cap, "%s", e.what());
    return kEvoTwitchSubscribersOnly;
  } catch (const twitch::ApiError& e) {
    snprintf(why, cap, "%s", e.what());
    return kEvoTwitchBadAnswer;
  } catch (const std::exception& e) {
    snprintf(why, cap, "%s", e.what());
    return kEvoTwitchFailed;
  } catch (...) {
    snprintf(why, cap, "unexpected exception");
    return kEvoTwitchFailed;
  }
}
