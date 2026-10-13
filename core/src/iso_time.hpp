#pragma once

#include <cstdlib>
#include <string>

// A small, dependency-free ISO 8601 (UTC) -> epoch-seconds parser, shared by
// the parsers that age or count from Twitch's timestamps (videos.cpp,
// browse.cpp). No <ctime>/timegm: this core also builds for the PS5, where a
// POSIX-only function like timegm isn't a safe bet. daysFromCivil is Howard
// Hinnant's well-known public-domain algorithm for days-since-epoch from a
// civil date. Inline functions in an anonymous namespace, as in
// json_helpers.hpp: no globals, no symbols to clash.

namespace twitch {
namespace {

inline long long daysFromCivil(int y, unsigned m, unsigned d) {
  y -= (m <= 2) ? 1 : 0;
  const long long era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? static_cast<unsigned>(-3) : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<long long>(doe) - 719468;
}

// Expects "YYYY-MM-DDTHH:MM:SS" at minimum (Twitch always sends this with a trailing "Z", but
// any suffix - fractional seconds, an offset - is simply ignored, and only UTC is understood).
inline bool parseIso8601(const std::string& iso, long long* epochSeconds) {
  if (iso.size() < 19) return false;
  auto digit = [&](size_t i) { return iso[i] >= '0' && iso[i] <= '9'; };
  for (size_t i : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u, 11u, 12u, 14u, 15u, 17u, 18u})
    if (!digit(i)) return false;
  if (iso[4] != '-' || iso[7] != '-' || iso[10] != 'T' || iso[13] != ':' || iso[16] != ':')
    return false;

  const int year = std::atoi(iso.substr(0, 4).c_str());
  const int month = std::atoi(iso.substr(5, 2).c_str());
  const int day = std::atoi(iso.substr(8, 2).c_str());
  const int hour = std::atoi(iso.substr(11, 2).c_str());
  const int minute = std::atoi(iso.substr(14, 2).c_str());
  const int second = std::atoi(iso.substr(17, 2).c_str());
  if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60)
    return false;

  const long long days = daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
  *epochSeconds = days * 86400LL + hour * 3600LL + minute * 60LL + second;
  return true;
}

}  // namespace
}  // namespace twitch
