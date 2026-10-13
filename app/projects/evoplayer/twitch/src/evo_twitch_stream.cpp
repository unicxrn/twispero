// evo_twitch_stream.cpp - SocketStream: TCP (and TLS) for the chat WebSocket.
// See evo_twitch_stream.h. Same calls as evo_net.c, so the console needs
// nothing new from its libc.
//
// Where the console allows it the socket stays non-blocking after connect, and
// every wait is a poll() against a monotonic deadline. That keeps readSome to
// its timeout with TLS too: SSL_read on a blocking socket can sit in recv()
// after a record that carries nothing for us (a session ticket, or half a
// record). The console's app sandbox refuses fcntl(O_NONBLOCK); FIONBIO is
// tried next, and if that is refused too the socket stays blocking: reads
// poll() first, and SO_RCVTIMEO (1 s) bounds a read that still has to wait,
// so readSome can run up to about a second past its timeout.
//
// No certificate verification, like the rest of the app. That is acceptable
// only while chat is read anonymously: stage 5 (login) must verify the server
// before a token is ever sent over one of these.

#include "evo_twitch_stream.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#if defined(__has_include)
#if __has_include(<netinet/tcp.h>)
#include <netinet/tcp.h>  // TCP_KEEPIDLE and friends, where they exist
#endif
#endif

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <climits>

#include "twitch/error.hpp"

namespace evo_twitch {

namespace {

constexpr int kConnectTimeoutMs = 5000;
constexpr int kHandshakeTimeoutMs = 10000;
constexpr int kWriteTimeoutMs = 10000;
constexpr int kBlockingSliceMs = 1000;  // SO_RCVTIMEO on a socket that stays blocking

long long nowMs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<long long>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// Waits until fd is ready for `events` or the deadline (a nowMs() value) has
// passed. 1 ready, 0 out of time, -1 poll failed. A signal doesn't cut the
// wait short.
int waitReady(int fd, short events, long long deadline) {
  for (;;) {
    const long long left = deadline - nowMs();
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = events;
    pfd.revents = 0;
    const int pr = poll(&pfd, 1, left <= 0 ? 0 : left > INT_MAX ? INT_MAX : static_cast<int>(left));
    if (pr > 0) return 1;
    if (pr == 0) {
      if (left <= 0) return 0;
      continue;  // woke a little early; look at the clock again
    }
    if (errno != EINTR) return -1;
  }
}

// Lets a dead connection (a router that forgot it, say) turn into an error
// within a few minutes instead of a chat that just goes quiet. Best effort.
void keepAlive(int fd) {
  int v = 1;
  setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &v, sizeof v);
#ifdef TCP_KEEPIDLE
  v = 60;
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &v, sizeof v);
#endif
#ifdef TCP_KEEPINTVL
  v = 15;
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &v, sizeof v);
#endif
#ifdef TCP_KEEPCNT
  v = 4;
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &v, sizeof v);
#endif
}

// Non-blocking through fcntl, or through ioctl(FIONBIO) where fcntl is
// refused (the console's app sandbox answers fcntl with EACCES). False when
// neither works; the socket is then still blocking.
bool setNonBlocking(int fd) {
#ifndef EVO_TWITCH_STREAM_FORCE_BLOCKING
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0) return true;
  int on = 1;
  if (ioctl(fd, FIONBIO, &on) == 0) return true;
#else
  (void)fd;  // the host test's way to exercise the blocking path
#endif
  return false;
}

void setTimeout(int fd, int option, int ms) {
  struct timeval tv;
  tv.tv_sec = ms / 1000;
  tv.tv_usec = (ms % 1000) * 1000;
  setsockopt(fd, SOL_SOCKET, option, &tv, sizeof tv);
}

// One address. A connected socket (non-blocking when the console allows it,
// see `blocking`), or -1 with the failed step and errno in `why`. A
// non-blocking connect is bounded by kConnectTimeoutMs; a blocking one sits in
// the kernel's SYN retries when nobody answers, as evo_net.c's does.
int connectOne(const struct addrinfo* rp, std::string& why, bool& blocking) {
  const int fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
  if (fd < 0) {
    why = "socket: errno " + std::to_string(errno);
    return -1;
  }

  blocking = !setNonBlocking(fd);
  bool ok = true;
#ifdef SO_NOSIGPIPE
  // Best effort, as in EVO's own sockets (evo_log_server.c):
  // chat_start() also ignores SIGPIPE for the whole app, which covers
  // OpenSSL's own writes too.
  const int one = 1;
  if (ok) setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
  if (ok && connect(fd, rp->ai_addr, rp->ai_addrlen) != 0) {
    if (blocking) {
      why = "connect: errno " + std::to_string(errno);
      ok = false;
    } else if (errno != EINPROGRESS && errno != EINTR) {
      why = "connect: errno " + std::to_string(errno);
      ok = false;
    } else if (waitReady(fd, POLLOUT, nowMs() + kConnectTimeoutMs) != 1) {
      why = "connect: no answer in " + std::to_string(kConnectTimeoutMs / 1000) + " s";
      ok = false;
    } else {
      int err = 0;
      socklen_t len = sizeof err;
      if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
        why = "connect: errno " + std::to_string(err ? err : errno);
        ok = false;
      }
    }
  }
  if (!ok) {
    ::close(fd);
    return -1;
  }
  if (blocking) {
    // Each blocking read gives up after kBlockingSliceMs, so readSome and the
    // TLS handshake can keep checking their deadlines; a write after
    // kWriteTimeoutMs.
    setTimeout(fd, SO_RCVTIMEO, kBlockingSliceMs);
    setTimeout(fd, SO_SNDTIMEO, kWriteTimeoutMs);
  }
  keepAlive(fd);
  return fd;
}

int clampToInt(std::size_t n) { return n > static_cast<std::size_t>(INT_MAX) ? INT_MAX : static_cast<int>(n); }

}  // namespace

void SocketStream::open(const std::string& host, int port, bool tls) {
  close();

  struct addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* res = nullptr;
  const int gai = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
  if (gai != 0 || !res) throw twitch::NetworkError("could not resolve " + host + " (" + std::to_string(gai) + ")");
  std::string why = "no address";
  for (const struct addrinfo* rp = res; rp && fd_ < 0; rp = rp->ai_next) fd_ = connectOne(rp, why, blocking_);
  freeaddrinfo(res);
  if (fd_ < 0)
    throw twitch::NetworkError("could not connect to " + host + ":" + std::to_string(port) + " (" + why + ")");

  if (!tls) return;
  ctx_ = SSL_CTX_new(TLS_client_method());
  if (ctx_) {
    // A server that drops the connection without a TLS close_notify reads as
    // "connection closed", like a plain socket, not as a TLS failure.
    SSL_CTX_set_options(ctx_, SSL_OP_IGNORE_UNEXPECTED_EOF);
    // writeAll retries a write that had to wait with the same bytes and
    // length; this keeps OpenSSL from also insisting on the same pointer.
    SSL_CTX_set_mode(ctx_, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    ssl_ = SSL_new(ctx_);
  }
  bool ok = ssl_ && SSL_set_tlsext_host_name(ssl_, host.c_str()) == 1 && SSL_set_fd(ssl_, fd_) == 1;
  why = ok ? "" : "setup";
  const long long deadline = nowMs() + kHandshakeTimeoutMs;
  while (ok) {
    ERR_clear_error();
    errno = 0;
    const int r = SSL_connect(ssl_);
    if (r == 1) break;
    const int err = SSL_get_error(ssl_, r);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
      ok = waitReady(fd_, err == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, deadline) == 1;
      if (!ok) why = "no answer in " + std::to_string(kHandshakeTimeoutMs / 1000) + " s";
    } else {
      // Which step failed, for the log and the chat's status line: OpenSSL's
      // own reason when it has one, else the error class and errno.
      char text[120] = "";
      const unsigned long e = ERR_peek_last_error();
      if (e) ERR_error_string_n(e, text, sizeof text);
      why = text[0] ? text : "error " + std::to_string(err) + ", errno " + std::to_string(errno);
      ok = false;
    }
  }
  if (!ok) {
    close();
    throw twitch::NetworkError("TLS handshake with " + host + " failed (" + why + ")");
  }
}

void SocketStream::writeAll(const char* data, std::size_t len) {
  if (fd_ < 0) throw twitch::NetworkError("not connected");
#ifdef MSG_NOSIGNAL
  const int flags = MSG_NOSIGNAL;
#else
  const int flags = 0;
#endif
  const long long deadline = nowMs() + kWriteTimeoutMs;
  while (len > 0) {
    short waitFor = 0;  // stays 0 for an error nothing can fix
    if (ssl_) {
      ERR_clear_error();
      const int w = SSL_write(ssl_, data, clampToInt(len));
      if (w > 0) {
        data += w;
        len -= static_cast<std::size_t>(w);
        continue;
      }
      const int err = SSL_get_error(ssl_, w);
      if (err == SSL_ERROR_WANT_WRITE)
        waitFor = POLLOUT;
      else if (err == SSL_ERROR_WANT_READ)
        waitFor = POLLIN;
    } else {
      const ssize_t w = send(fd_, data, len, flags);
      if (w > 0) {
        data += w;
        len -= static_cast<std::size_t>(w);
        continue;
      }
      if (w < 0 && errno == EINTR) continue;
      if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) waitFor = POLLOUT;
    }
    if (waitFor == 0 || waitReady(fd_, waitFor, deadline) != 1)
      throw twitch::NetworkError("connection lost while sending");
  }
}

std::size_t SocketStream::readSome(char* buf, std::size_t cap, int timeoutMs) {
  if (fd_ < 0) throw twitch::NetworkError("not connected");
  if (cap == 0) return 0;  // recv would return 0, which means "closed"

  // Non-blocking: a read comes straight back with data or "nothing yet"; only
  // then is there something to wait for. Blocking: wait for data first, so the
  // read itself rarely has to (SO_RCVTIMEO bounds it when it does).
  const long long deadline = nowMs() + (timeoutMs > 0 ? timeoutMs : 0);
  for (;;) {
    if (blocking_ && !(ssl_ && SSL_pending(ssl_) > 0)) {
      const int ready = waitReady(fd_, POLLIN, deadline);
      if (ready == 0) return 0;
      if (ready < 0) throw twitch::NetworkError("connection lost while waiting for data");
    }
    short waitFor;
    if (ssl_) {
      ERR_clear_error();
      const int r = SSL_read(ssl_, buf, clampToInt(cap));
      if (r > 0) return static_cast<std::size_t>(r);
      const int err = SSL_get_error(ssl_, r);
      if (err == SSL_ERROR_ZERO_RETURN) throw twitch::NetworkError("connection closed");
      if (err == SSL_ERROR_WANT_READ)  // nothing yet, half a record, or a record with no data
        waitFor = POLLIN;
      else if (err == SSL_ERROR_WANT_WRITE)
        waitFor = POLLOUT;
      else
        throw twitch::NetworkError("TLS read failed");
    } else {
      const ssize_t r = recv(fd_, buf, cap, 0);
      if (r > 0) return static_cast<std::size_t>(r);
      if (r == 0) throw twitch::NetworkError("connection closed");
      if (errno == EINTR) continue;
      if (errno != EAGAIN && errno != EWOULDBLOCK) throw twitch::NetworkError("connection lost while receiving");
      waitFor = POLLIN;
    }
    if (blocking_ && waitFor == POLLIN) {
      if (nowMs() >= deadline) return 0;
      continue;  // the poll at the top waits
    }
    const int ready = waitReady(fd_, waitFor, deadline);
    if (ready == 0) return 0;
    if (ready < 0) throw twitch::NetworkError("connection lost while waiting for data");
  }
}

void SocketStream::close() noexcept {
  // No SSL_shutdown: it waits on the network, and the server doesn't need it.
  if (ssl_) SSL_free(ssl_);
  if (ctx_) SSL_CTX_free(ctx_);
  if (fd_ >= 0) ::close(fd_);
  ssl_ = nullptr;
  ctx_ = nullptr;
  fd_ = -1;
}

}  // namespace evo_twitch
