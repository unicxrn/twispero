#include "twitch/curl/curl_http.hpp"

#include <curl/curl.h>

#include <memory>
#include <mutex>
#include <string>

#include "twitch/error.hpp"

namespace twitch {
namespace {

struct WriteCtx {
  std::string* body;
  size_t limit;
  bool exceeded = false;
};

size_t appendBody(char* data, size_t size, size_t count, void* userdata) {
  auto* ctx = static_cast<WriteCtx*>(userdata);
  size_t incoming = size * count;
  if (ctx->body->size() + incoming > ctx->limit) {
    ctx->exceeded = true;
    return 0;
  }
  ctx->body->append(data, incoming);
  return incoming;
}

}  // namespace

void curlGlobalInit() {
  static std::once_flag once;
  std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

CurlHttpClient::CurlHttpClient(size_t maxBodyBytes)
    : handle_(curl_easy_init()), maxBodyBytes_(maxBodyBytes) {
  if (handle_ == nullptr) throw NetworkError("curl_easy_init failed");
}

CurlHttpClient::~CurlHttpClient() { curl_easy_cleanup(handle_); }

HttpResponse CurlHttpClient::send(const HttpRequest& request) {
  if (request.method != "GET" && request.method != "POST")
    throw NetworkError("unsupported HTTP method: " + request.method);

  curl_easy_reset(handle_);
  std::string body;
  WriteCtx ctx{&body, maxBodyBytes_, false};
  std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(nullptr, &curl_slist_free_all);
  for (const auto& [key, value] : request.headers) {
    std::string line = key + ": " + value;
    curl_slist* next = curl_slist_append(headers.get(), line.c_str());
    if (next == nullptr) throw NetworkError("out of memory building headers");
    headers.release();
    headers.reset(next);
  }

  curl_easy_setopt(handle_, CURLOPT_URL, request.url.c_str());
  curl_easy_setopt(handle_, CURLOPT_HTTPHEADER, headers.get());
  curl_easy_setopt(handle_, CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt(handle_, CURLOPT_REDIR_PROTOCOLS_STR, "https");
  curl_easy_setopt(handle_, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(handle_, CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(handle_, CURLOPT_TIMEOUT, 15L);
  curl_easy_setopt(handle_, CURLOPT_USERAGENT, "ps5-twitch/0.1");
  curl_easy_setopt(handle_, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(handle_, CURLOPT_WRITEFUNCTION, appendBody);
  curl_easy_setopt(handle_, CURLOPT_WRITEDATA, &ctx);
  if (request.method == "POST") {
    curl_easy_setopt(handle_, CURLOPT_POST, 1L);
    curl_easy_setopt(handle_, CURLOPT_POSTFIELDS, request.body.c_str());
    curl_easy_setopt(handle_, CURLOPT_POSTFIELDSIZE_LARGE,
                      static_cast<curl_off_t>(request.body.size()));
  }

  CURLcode rc = curl_easy_perform(handle_);
  if (rc == CURLE_WRITE_ERROR && ctx.exceeded) {
    throw NetworkError("HTTP response larger than " + std::to_string(maxBodyBytes_) + " bytes");
  }
  if (rc != CURLE_OK) throw NetworkError(std::string("HTTP request failed: ") + curl_easy_strerror(rc));

  long status = 0;
  curl_easy_getinfo(handle_, CURLINFO_RESPONSE_CODE, &status);
  return HttpResponse{static_cast<int>(status), std::move(body)};
}

}  // namespace twitch
