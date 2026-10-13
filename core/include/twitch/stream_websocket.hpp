#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "twitch/bytestream.hpp"
#include "twitch/websocket.hpp"

namespace twitch {

std::string base64Encode(const std::string& bytes);

// An RFC 6455 client over a ByteStream, for "ws://host[:port][/path]" and
// "wss://host[:port][/path]". Text messages only; it answers pings itself.
// `seed` feeds the handshake key and the frame masks, which have to vary,
// not to be secret. Sec-WebSocket-Accept is not verified (no SHA-1 here, and
// TLS isn't verified on the console either). One thread at a time.
class StreamWebSocket : public WebSocket {
 public:
  static constexpr std::size_t kMaxMessage = 1u << 20;  // 1 MiB
  static constexpr int kHandshakeTimeoutMs = 10000;

  StreamWebSocket(ByteStream& stream, std::uint32_t seed);
  StreamWebSocket(const StreamWebSocket&) = delete;
  StreamWebSocket& operator=(const StreamWebSocket&) = delete;

  void connect(const std::string& url) override;
  void sendText(const std::string& text) override;
  // Waits up to timeoutMs for more bytes, at most once; a message that is
  // still arriving comes back on a later call.
  std::optional<std::string> receiveText(int timeoutMs) override;
  void close() override;

 private:
  void sendFrame(int opcode, const std::string& payload);
  bool takeFrame(int& opcode, bool& fin, std::string& payload);
  std::uint32_t nextRandom();
  // Marks the connection closed, best-effort sends a close frame carrying
  // closeCode (skipped when closeCode == 0), and throws NetworkError(why).
  [[noreturn]] void fail(const std::string& why, int closeCode);
  // Clears open_, buf_, partial_, inText_ and inBinary_.
  void resetState();

  ByteStream& stream_;
  std::uint32_t state_;
  std::string buf_;      // bytes received but not parsed yet
  std::string partial_;  // a fragmented text message so far; binary payloads
                          // are skipped and never stored here
  bool inText_ = false;    // partial_ holds the start of a text message
  bool inBinary_ = false;  // a binary message (ignored) is being skipped
  bool open_ = false;
};

}  // namespace twitch
