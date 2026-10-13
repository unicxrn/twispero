#pragma once
// evo_twitch_stream.h - a TCP connection, optionally TLS, as a twitch::ByteStream.
//
// Plain POSIX sockets, poll() and OpenSSL, so the same file builds for the
// console and for the PC (twitch/tests/chat_host_test.sh). The socket stays
// non-blocking, so reads and writes keep to their timeouts, TLS included:
// readSome waits at most timeoutMs, and each writeAll up to 10 s. Connect
// waits up to 5 s per address, the TLS handshake up to 10 s.
// One thread at a time. Throws twitch::NetworkError like the interface says.
//
// TLS is NOT verified (no CA bundle on the console, like the rest of the app).
// Fine for reading chat anonymously; the login stage must add verification
// before any token goes over one of these.

#include <cstddef>
#include <string>

#include "twitch/bytestream.hpp"

struct ssl_ctx_st;
struct ssl_st;

namespace evo_twitch {

class SocketStream final : public twitch::ByteStream {
 public:
  SocketStream() = default;
  ~SocketStream() override { close(); }
  SocketStream(const SocketStream&) = delete;
  SocketStream& operator=(const SocketStream&) = delete;

  void open(const std::string& host, int port, bool tls) override;
  void writeAll(const char* data, std::size_t len) override;
  std::size_t readSome(char* buf, std::size_t cap, int timeoutMs) override;
  void close() noexcept override;

 private:
  int fd_ = -1;
  // The console's app sandbox refuses fcntl(O_NONBLOCK) (EACCES). When
  // FIONBIO is refused too, the socket stays blocking: reads poll() first and
  // SO_RCVTIMEO/SO_SNDTIMEO bound each call, as in EVO's own evo_net.c.
  bool blocking_ = false;
  ssl_ctx_st* ctx_ = nullptr;
  ssl_st* ssl_ = nullptr;
};

}  // namespace evo_twitch
