#include "twitch/url.hpp"

namespace twitch {

std::string percentEncode(std::string_view in) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  out.reserve(in.size() * 3);
  for (unsigned char c : in) {
    bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                      (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~';
    if (unreserved) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 0x0F]);
    }
  }
  return out;
}

std::string buildQuery(const std::vector<std::pair<std::string, std::string>>& pairs) {
  std::string out;
  for (const auto& [key, value] : pairs) {
    if (!out.empty()) out.push_back('&');
    out += percentEncode(key);
    out.push_back('=');
    out += percentEncode(value);
  }
  return out;
}

}  // namespace twitch
