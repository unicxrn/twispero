#pragma once
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace twitch {

// RFC 3986 percent-encoding: A-Z a-z 0-9 - . _ ~ stay, everything else becomes %XX.
std::string percentEncode(std::string_view in);

// "k1=v1&k2=v2" with both keys and values percent-encoded, in the given order.
std::string buildQuery(const std::vector<std::pair<std::string, std::string>>& pairs);

}  // namespace twitch
