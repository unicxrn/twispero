#pragma once

#include <climits>
#include <cmath>
#include <string>

#include <nlohmann/json.hpp>

// Shared JSON helpers for the core's GraphQL response parsers (streams.cpp,
// browse.cpp). They never throw nlohmann exceptions: a wrong type just gives
// the default. "Missing" is nullptr rather than a shared static json, which
// would need a dynamic initializer at load time (fragile under EVO's own
// CRT). Kept as inline functions in an anonymous namespace so each
// translation unit that includes this header gets its own internal-linkage
// copy: no globals, no symbols to clash across .cpp files.

namespace twitch {
namespace {

// The object under key, or nullptr when obj is missing or not an object, or
// key is absent or not an object.
inline const nlohmann::json* objectOr(const nlohmann::json* obj, const char* key) {
  if (!obj || !obj->is_object()) return nullptr;
  auto it = obj->find(key);
  if (it == obj->end() || !it->is_object()) return nullptr;
  return &*it;
}

inline std::string stringOr(const nlohmann::json* obj, const char* key) {
  if (!obj || !obj->is_object()) return {};
  auto it = obj->find(key);
  if (it == obj->end() || !it->is_string()) return {};
  return it->get<std::string>();
}

// Viewer counts can't be negative; clamps to [0, INT_MAX] so an out-of-range
// or non-finite value from Twitch can never be UB. Returns `def` when the
// field itself is missing, null or the wrong type (a present-but-negative or
// non-finite value still clamps to 0, same as intOr).
inline int intOrDefault(const nlohmann::json* obj, const char* key, int def) {
  if (!obj || !obj->is_object()) return def;
  auto it = obj->find(key);
  if (it == obj->end() || !it->is_number()) return def;
  const auto& v = *it;
  if (v.is_number_integer() || v.is_number_unsigned()) {
    long long n = v.get<long long>();
    if (n < 0) return 0;
    if (n > static_cast<long long>(INT_MAX)) return INT_MAX;
    return static_cast<int>(n);
  }
  double d = v.get<double>();
  if (std::isnan(d) || d <= 0) return 0;
  if (d >= static_cast<double>(INT_MAX)) return INT_MAX;
  return static_cast<int>(d);
}

inline int intOr(const nlohmann::json* obj, const char* key) { return intOrDefault(obj, key, 0); }

}  // namespace
}  // namespace twitch
