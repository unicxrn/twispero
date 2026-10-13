#include "twitch/curl/curl_websocket.hpp"

#include <curl/curl.h>
#include <poll.h>

#include <chrono>

#include "twitch/error.hpp"

namespace twitch {

CurlWebSocket::CurlWebSocket(size_t maxMessageBytes)
    : handle_(curl_easy_init()), maxMessageBytes_(maxMessageBytes) {
  if (handle_ == nullptr) throw NetworkError("curl_easy_init failed");
}

CurlWebSocket::~CurlWebSocket() { curl_easy_cleanup(handle_); }

void CurlWebSocket::connect(const std::string& url) {
  curl_easy_setopt(handle_, CURLOPT_URL, url.c_str());
  curl_easy_setopt(handle_, CURLOPT_PROTOCOLS_STR, "wss");
  curl_easy_setopt(handle_, CURLOPT_CONNECT_ONLY, 2L);  // 2 = WebSocket upgrade
  curl_easy_setopt(handle_, CURLOPT_CONNECTTIMEOUT, 15L);
  CURLcode rc = curl_easy_perform(handle_);
  if (rc != CURLE_OK)
    throw NetworkError(std::string("WebSocket connect failed: ") + curl_easy_strerror(rc));
}

bool CurlWebSocket::waitSocket(short events, int timeoutMs) {
  curl_socket_t sock = CURL_SOCKET_BAD;
  curl_easy_getinfo(handle_, CURLINFO_ACTIVESOCKET, &sock);
  if (sock == CURL_SOCKET_BAD) throw NetworkError("WebSocket is not connected");
  pollfd pfd{static_cast<int>(sock), events, 0};
  int n = ::poll(&pfd, 1, timeoutMs);
  if (n < 0) throw NetworkError("poll failed");
  return n > 0;
}

void CurlWebSocket::sendText(const std::string& text) {
  size_t offset = 0;
  while (offset < text.size()) {
    size_t sent = 0;
    CURLcode rc = curl_ws_send(handle_, text.data() + offset, text.size() - offset, &sent, 0,
                               CURLWS_TEXT);
    if (rc == CURLE_AGAIN) {
      if (!waitSocket(POLLOUT, 15000)) throw NetworkError("WebSocket send timed out");
      continue;
    }
    if (rc != CURLE_OK)
      throw NetworkError(std::string("WebSocket send failed: ") + curl_easy_strerror(rc));
    offset += sent;
  }
}

std::optional<std::string> CurlWebSocket::receiveText(int timeoutMs) {
  using Clock = std::chrono::steady_clock;
  auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
  // Once part of a message has arrived, allow at most 10 s more for the rest.
  auto partialDeadline = Clock::time_point::max();
  std::string message;
  char buf[16384];

  while (true) {
    size_t got = 0;
    const curl_ws_frame* meta = nullptr;
    CURLcode rc = curl_ws_recv(handle_, buf, sizeof buf, &got, &meta);

    if (rc == CURLE_AGAIN) {
      auto now = Clock::now();
      auto until = message.empty() ? deadline : partialDeadline;
      if (now >= until) {
        if (message.empty()) return std::nullopt;
        throw NetworkError("WebSocket message stalled mid-frame");
      }
      int left = static_cast<int>(
          std::chrono::duration_cast<std::chrono::milliseconds>(until - now).count());
      if (!waitSocket(POLLIN, left) && message.empty()) return std::nullopt;
      continue;
    }
    if (rc == CURLE_GOT_NOTHING) throw NetworkError("WebSocket closed by server");
    if (rc != CURLE_OK)
      throw NetworkError(std::string("WebSocket receive failed: ") + curl_easy_strerror(rc));
    if (meta == nullptr) continue;
    if (meta->flags & CURLWS_CLOSE) throw NetworkError("WebSocket closed by server");
    if (!(meta->flags & (CURLWS_TEXT | CURLWS_CONT))) continue;  // ignore ping/pong/binary

    if (message.size() + got > maxMessageBytes_)
      throw NetworkError("WebSocket message larger than " + std::to_string(maxMessageBytes_) + " bytes");
    if (message.empty()) partialDeadline = Clock::now() + std::chrono::seconds(10);
    message.append(buf, got);
    if (meta->bytesleft == 0 && !(meta->flags & CURLWS_CONT)) return message;
  }
}

void CurlWebSocket::close() {
  size_t sent = 0;
  curl_ws_send(handle_, "", 0, &sent, 0, CURLWS_CLOSE);
}

}  // namespace twitch
