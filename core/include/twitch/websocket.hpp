#pragma once
#include <optional>
#include <string>

namespace twitch {

// Text-only WebSocket client, implemented per platform.
class WebSocket {
 public:
  virtual ~WebSocket() = default;
  virtual void connect(const std::string& url) = 0;  // throws NetworkError
  virtual void sendText(const std::string& text) = 0;  // throws NetworkError
  // Waits up to timeoutMs for one complete text message.
  // Returns nullopt on timeout; throws NetworkError when the connection is closed.
  virtual std::optional<std::string> receiveText(int timeoutMs) = 0;
  virtual void close() = 0;
};

}  // namespace twitch
