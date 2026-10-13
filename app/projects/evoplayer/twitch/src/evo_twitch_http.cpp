// twitch::HttpClient on EVO's network code (addons/src/evo_net.c).
//
// evo_net's synchronous calls keep their state on the caller's stack and guard
// the shared SSL_CTX with a mutex, so one client serves every thread.

#include <pthread.h>
#include <stdlib.h>

#include <new>
#include <string>
#include <vector>

#include "evo_error.h"
#include "evo_net.h"
#include "evo_twitch_internal.h"
#include "twitch/error.hpp"
#include "twitch/http.hpp"

namespace {

// evo_net hands back a malloc'd body that the caller frees, also when send()
// leaves by an exception.
struct NetResponse {
  evo_net_response_t r{};
  ~NetResponse() { free(r.body); }
};

class EvoHttpClient final : public twitch::HttpClient {
 public:
  explicit EvoHttpClient(unsigned flags) : flags_(flags) {}

  twitch::HttpResponse send(const twitch::HttpRequest& request) override {
    const bool post = request.method == "POST";
    if (!post && request.method != "GET") throw twitch::NetworkError("unsupported HTTP method");

    // evo_net writes Host, User-Agent, Accept and Connection itself, and for a
    // POST also Content-Length, plus "Content-Type: application/json" unless
    // the request names its own type (a form POST does).
    std::vector<std::string> lines;
    lines.reserve(request.headers.size());
    for (const auto& h : request.headers) lines.push_back(h.first + ": " + h.second);
    std::vector<const char*> headers;
    headers.reserve(lines.size());
    for (const auto& line : lines) headers.push_back(line.c_str());

    evo_net_request_t req{};
    req.method = post ? "POST" : "GET";
    req.url = request.url.c_str();
    req.body = post ? request.body.c_str() : nullptr;
    req.headers = headers.empty() ? nullptr : headers.data();
    req.header_count = static_cast<int>(headers.size());
    req.flags = flags_;

    NetResponse res;
    const int rc = evo_net_http_request_sync(&req, &res.r);

    // EVO_NET_ERR_HTTP is a real answer with a status >= 400; anything else
    // negative means no answer at all. The description names the host only,
    // never the path or query (they carry Twitch's signed token).
    if (rc != EVO_NET_OK && rc != EVO_NET_ERR_HTTP) throw twitch::NetworkError(res.r.error);
    // A connection that closed before any status line comes back as
    // EVO_NET_ERR_HTTP with status 0. That is no response either.
    if (res.r.status <= 0) {
      char host[EVO_NET_MAX_HOST];
      evo_error_url_host(request.url.c_str(), host, sizeof host);
      throw twitch::NetworkError(std::string(host) + (host[0] ? ": " : "") +
                                 "the server closed the connection without answering");
    }

    twitch::HttpResponse response;
    response.status = res.r.status;
    if (res.r.body) response.body.assign(res.r.body, res.r.body_len);
    return response;
  }

 private:
  const unsigned flags_;
};

// EvoHttpClient has a vtable and, through HttpClient's virtual destructor, a
// non-trivial destructor: a plain file-scope object would need a global
// initializer that registers an exit-time destructor. EVO keeps static
// initialization trivial and does not tear objects down at exit (see
// Application::getInstance), so each client is placement-constructed into
// static storage on first use and never destroyed. Everything here is
// trivially constructible.
pthread_mutex_t g_http_mx = PTHREAD_MUTEX_INITIALIZER;
alignas(EvoHttpClient) unsigned char g_http_storage[sizeof(EvoHttpClient)];
alignas(EvoHttpClient) unsigned char g_http_verified_storage[sizeof(EvoHttpClient)];
twitch::HttpClient* g_http = nullptr;
twitch::HttpClient* g_http_verified = nullptr;

}  // namespace

twitch::HttpClient& evoTwitchHttp() {
  pthread_mutex_lock(&g_http_mx);
  if (!g_http) g_http = new (g_http_storage) EvoHttpClient(0);
  twitch::HttpClient* http = g_http;
  pthread_mutex_unlock(&g_http_mx);
  return *http;
}

twitch::HttpClient& evoTwitchHttpVerified() {
  pthread_mutex_lock(&g_http_mx);
  if (!g_http_verified) {
    g_http_verified = new (g_http_verified_storage)
        EvoHttpClient(EVO_NET_VERIFY_TLS | EVO_NET_NO_CROSS_HOST_HEADERS);
  }
  twitch::HttpClient* http = g_http_verified;
  pthread_mutex_unlock(&g_http_mx);
  return *http;
}
