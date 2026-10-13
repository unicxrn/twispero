#pragma once
#include <cstddef>

#include "twitch/websocket.hpp"

typedef void CURL;

namespace twitch {

// WebSocket over libcurl's curl_ws_* API (CONNECT_ONLY mode), wss:// only. Text frames only;
// curl answers WebSocket PING frames itself (IRC-level PING is the ChatClient's job).
class CurlWebSocket : public WebSocket {
 public:
  explicit CurlWebSocket(size_t maxMessageBytes = 1024 * 1024);
  ~CurlWebSocket() override;
  CurlWebSocket(const CurlWebSocket&) = delete;
  CurlWebSocket& operator=(const CurlWebSocket&) = delete;

  void connect(const std::string& url) override;
  void sendText(const std::string& text) override;
  std::optional<std::string> receiveText(int timeoutMs) override;
  void close() override;

 private:
  // Waits until the socket is readable (events = POLLIN) or writable (POLLOUT).
  // Returns false on timeout.
  bool waitSocket(short events, int timeoutMs);
  CURL* handle_;
  size_t maxMessageBytes_;
};

}  // namespace twitch
