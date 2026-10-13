#pragma once
#include <string>
#include <utility>
#include <vector>

namespace twitch {

struct HttpRequest {
  std::string method = "GET";
  std::string url;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
};

struct HttpResponse {
  int status = 0;
  std::string body;
};

// Implemented per platform (libcurl on Linux, EVO's network code on PS5).
// Implementations only need to support the methods "GET" and "POST".
// send() returns any HTTP status, including 4xx/5xx.
// It throws NetworkError only when no HTTP response was received at all.
class HttpClient {
 public:
  virtual ~HttpClient() = default;
  virtual HttpResponse send(const HttpRequest& request) = 0;
};

}  // namespace twitch
