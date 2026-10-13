#pragma once
#include <cstddef>
#include <string>

namespace twitch {

// A connected byte stream (TCP, optionally TLS), implemented per platform.
class ByteStream {
 public:
  virtual ~ByteStream() = default;
  // Connects to host:port, with TLS when tls is true. Throws NetworkError.
  virtual void open(const std::string& host, int port, bool tls) = 0;
  // Sends every byte. Throws NetworkError.
  virtual void writeAll(const char* data, std::size_t len) = 0;
  // Waits up to timeoutMs for data. Returns how many bytes it read (> 0), or 0
  // when nothing arrived in time. Throws NetworkError when the other side
  // closed the connection or on any error.
  virtual std::size_t readSome(char* buf, std::size_t cap, int timeoutMs) = 0;
  // Closes the connection. Safe to call twice, and on a stream never opened.
  virtual void close() noexcept = 0;
};

}  // namespace twitch
