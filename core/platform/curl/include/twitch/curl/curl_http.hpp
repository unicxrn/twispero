#pragma once
#include <cstddef>

#include "twitch/http.hpp"

typedef void CURL;

namespace twitch {

class CurlHttpClient : public HttpClient {
 public:
  explicit CurlHttpClient(size_t maxBodyBytes = 8 * 1024 * 1024);
  ~CurlHttpClient() override;
  CurlHttpClient(const CurlHttpClient&) = delete;
  CurlHttpClient& operator=(const CurlHttpClient&) = delete;

  HttpResponse send(const HttpRequest& request) override;

 private:
  CURL* handle_;
  size_t maxBodyBytes_;
};

// Call once at program start, before any curl-based client is created.
void curlGlobalInit();

}  // namespace twitch
