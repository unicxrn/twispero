#include "twitch/login.hpp"

#include <algorithm>
#include <cctype>

#include "twitch/error.hpp"

namespace twitch {
namespace {

bool isLoginChar(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

// Shows the original input in the error message, with any non-printable byte as '?'.
std::string printable(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) out.push_back(std::isprint(c) ? static_cast<char>(c) : '?');
  return out;
}

}  // namespace

std::string normalizeLogin(const std::string& login) {
  std::string lower = login;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  bool valid = !lower.empty() && lower.size() <= 25 &&
               std::all_of(lower.begin(), lower.end(), isLoginChar);
  if (!valid) throw NotFoundError("invalid channel name: " + printable(login));
  return lower;
}

}  // namespace twitch
