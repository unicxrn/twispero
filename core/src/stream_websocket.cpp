#include "twitch/stream_websocket.hpp"

#include "twitch/error.hpp"

namespace twitch {

namespace {

const char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Frame opcodes (RFC 6455 section 5.2).
constexpr int kContinuation = 0x0;
constexpr int kText = 0x1;
constexpr int kBinary = 0x2;
constexpr int kClose = 0x8;
constexpr int kPing = 0x9;
constexpr int kPong = 0xA;

struct WsUrl {
  std::string host, path;
  int port = 0;
  bool tls = false;
};

WsUrl parseWsUrl(const std::string& url) {
  WsUrl u;
  std::string rest;
  if (url.rfind("wss://", 0) == 0) {
    u.tls = true;
    u.port = 443;
    rest = url.substr(6);
  } else if (url.rfind("ws://", 0) == 0) {
    u.port = 80;
    rest = url.substr(5);
  } else {
    throw NetworkError("not a WebSocket URL: " + url);
  }
  const std::size_t slash = rest.find('/');
  const std::string hostPort = rest.substr(0, slash);
  u.path = slash == std::string::npos ? "/" : rest.substr(slash);
  const std::size_t colon = hostPort.find(':');
  u.host = hostPort.substr(0, colon);
  if (colon != std::string::npos) {
    const std::string digits = hostPort.substr(colon + 1);
    long port = digits.empty() ? -1 : 0;
    for (char c : digits) {
      if (c < '0' || c > '9' || port > 65535) {
        port = -1;
        break;
      }
      port = port * 10 + (c - '0');
    }
    if (port <= 0 || port > 65535) throw NetworkError("bad port in WebSocket URL: " + url);
    u.port = static_cast<int>(port);
  }
  if (u.host.empty()) throw NetworkError("no host in WebSocket URL: " + url);
  return u;
}

char asciiLower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

std::string toLowerAscii(std::string s) {
  for (char& c : s) c = asciiLower(c);
  return s;
}

std::string trim(const std::string& s) {
  const std::size_t a = s.find_first_not_of(" \t");
  if (a == std::string::npos) return "";
  const std::size_t b = s.find_last_not_of(" \t");
  return s.substr(a, b - a + 1);
}

// Keeps a close reason from forging a log line (a raw CR/LF) or truncating
// one (a raw NUL, which also cuts short the C string that what() returns)
// once it ends up inside an exception message.
std::string sanitizeForMessage(std::string s) {
  for (char& c : s) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20 || u > 0x7E) c = '?';
  }
  return s;
}

}  // namespace

std::string base64Encode(const std::string& in) {
  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);
  std::size_t i = 0;
  auto byte = [&](std::size_t k) { return static_cast<unsigned>(static_cast<unsigned char>(in[k])); };
  for (; i + 2 < in.size(); i += 3) {
    const unsigned v = byte(i) << 16 | byte(i + 1) << 8 | byte(i + 2);
    out += kBase64[(v >> 18) & 63];
    out += kBase64[(v >> 12) & 63];
    out += kBase64[(v >> 6) & 63];
    out += kBase64[v & 63];
  }
  if (i + 1 == in.size()) {
    const unsigned v = byte(i) << 16;
    out += kBase64[(v >> 18) & 63];
    out += kBase64[(v >> 12) & 63];
    out += "==";
  } else if (i + 2 == in.size()) {
    const unsigned v = byte(i) << 16 | byte(i + 1) << 8;
    out += kBase64[(v >> 18) & 63];
    out += kBase64[(v >> 12) & 63];
    out += kBase64[(v >> 6) & 63];
    out += '=';
  }
  return out;
}

StreamWebSocket::StreamWebSocket(ByteStream& stream, std::uint32_t seed)
    : stream_(stream), state_(seed ? seed : 0x9E3779B9u) {}

std::uint32_t StreamWebSocket::nextRandom() {  // xorshift32
  state_ ^= state_ << 13;
  state_ ^= state_ >> 17;
  state_ ^= state_ << 5;
  return state_;
}

void StreamWebSocket::connect(const std::string& url) {
  const WsUrl u = parseWsUrl(url);
  resetState();
  stream_.open(u.host, u.port, u.tls);

  std::string key;
  for (int i = 0; i < 16; ++i) key += static_cast<char>(nextRandom() & 0xFF);
  const bool defaultPort = u.port == (u.tls ? 443 : 80);
  const std::string request = "GET " + u.path + " HTTP/1.1\r\n" + "Host: " + u.host +
                              (defaultPort ? "" : ":" + std::to_string(u.port)) + "\r\n" +
                              "Upgrade: websocket\r\n"
                              "Connection: Upgrade\r\n"
                              "Sec-WebSocket-Key: " +
                              base64Encode(key) + "\r\n" + "Sec-WebSocket-Version: 13\r\n\r\n";
  stream_.writeAll(request.data(), request.size());

  // Bounded by whichever limit is hit first: 10 empty reads (waitedMs
  // reaches kHandshakeTimeoutMs, 10 s) or 64 reads in total, so a server
  // that trickles a byte or two per read - never idle long enough to trip
  // the first limit, never finishing the header - can't hold connect()
  // for more than about 64 s. Deliberately clock-free: both limits count
  // reads/waits, not wall-clock time.
  int waitedMs = 0;
  int reads = 0;
  std::size_t headerEnd;
  while ((headerEnd = buf_.find("\r\n\r\n")) == std::string::npos) {
    if (buf_.size() > 8192) throw NetworkError("WebSocket handshake: the answer is too long");
    if (reads >= 64) throw NetworkError("WebSocket handshake: too many reads without finishing");
    char chunk[2048];
    const std::size_t n = stream_.readSome(chunk, sizeof chunk, 1000);
    ++reads;
    if (n == 0) {
      waitedMs += 1000;
      if (waitedMs >= kHandshakeTimeoutMs) throw NetworkError("WebSocket handshake timed out");
      continue;
    }
    buf_.append(chunk, n);
  }
  const std::string status = buf_.substr(0, buf_.find("\r\n"));
  // Accept either a reason phrase ("HTTP/1.1 101 Switching Protocols") or a
  // status line with none at all ("HTTP/1.1 101" and nothing else).
  if (status != "HTTP/1.1 101" && status.rfind("HTTP/1.1 101 ", 0) != 0)
    throw NetworkError("WebSocket handshake refused: " + status);

  // Require an Upgrade: websocket header, matched case-insensitively; we do
  // not verify Sec-WebSocket-Accept (see the class comment).
  const std::string headers = buf_.substr(0, headerEnd);
  bool sawUpgrade = false;
  bool firstLine = true;
  std::size_t pos = 0;
  for (;;) {
    const std::size_t eol = headers.find("\r\n", pos);
    const std::string line = (eol == std::string::npos) ? headers.substr(pos) : headers.substr(pos, eol - pos);
    if (!firstLine) {
      const std::size_t colon = line.find(':');
      if (colon != std::string::npos) {
        const std::string name = toLowerAscii(trim(line.substr(0, colon)));
        const std::string value = toLowerAscii(trim(line.substr(colon + 1)));
        if (name == "upgrade" && value == "websocket") sawUpgrade = true;
      }
    }
    firstLine = false;
    if (eol == std::string::npos) break;
    pos = eol + 2;
  }
  if (!sawUpgrade) throw NetworkError("WebSocket handshake missing an Upgrade: websocket header");

  buf_.erase(0, headerEnd + 4);
  open_ = true;
}

void StreamWebSocket::sendFrame(int opcode, const std::string& payload) {
  std::string f;
  f += static_cast<char>(0x80 | opcode);
  const unsigned long long n = payload.size();
  if (n < 126) {
    f += static_cast<char>(0x80 | n);
  } else if (n <= 0xFFFF) {
    f += static_cast<char>(0x80 | 126);
    f += static_cast<char>((n >> 8) & 0xFF);
    f += static_cast<char>(n & 0xFF);
  } else {
    f += static_cast<char>(0x80 | 127);
    for (int s = 56; s >= 0; s -= 8) f += static_cast<char>((n >> s) & 0xFF);
  }
  const std::uint32_t m = nextRandom();
  const unsigned char mask[4] = {static_cast<unsigned char>(m >> 24), static_cast<unsigned char>(m >> 16),
                                 static_cast<unsigned char>(m >> 8), static_cast<unsigned char>(m)};
  f.append(reinterpret_cast<const char*>(mask), 4);
  for (std::size_t i = 0; i < payload.size(); ++i) f += static_cast<char>(payload[i] ^ mask[i & 3]);
  stream_.writeAll(f.data(), f.size());
}

void StreamWebSocket::sendText(const std::string& text) {
  if (!open_) throw NetworkError("WebSocket is not connected");
  try {
    sendFrame(kText, text);
  } catch (const NetworkError&) {
    resetState();
    throw;
  }
}

void StreamWebSocket::resetState() {
  open_ = false;
  buf_.clear();
  partial_.clear();
  inText_ = false;
  inBinary_ = false;
}

void StreamWebSocket::fail(const std::string& why, int closeCode) {
  resetState();
  if (closeCode != 0) {
    std::string payload;
    payload += static_cast<char>((closeCode >> 8) & 0xFF);
    payload += static_cast<char>(closeCode & 0xFF);
    try {
      sendFrame(kClose, payload);
    } catch (const NetworkError&) {
    }
  }
  throw NetworkError(why);
}

bool StreamWebSocket::takeFrame(int& opcode, bool& fin, std::string& payload) {
  if (buf_.size() < 2) return false;
  const unsigned char b0 = static_cast<unsigned char>(buf_[0]);
  const unsigned char b1 = static_cast<unsigned char>(buf_[1]);
  if (b0 & 0x70) fail("WebSocket: reserved bits set", 1002);
  const int op = b0 & 0x0F;
  if ((op >= 0x3 && op <= 0x7) || op >= 0xB) fail("WebSocket: reserved opcode", 1002);
  if (b1 & 0x80) fail("WebSocket: server sent a masked frame", 1002);
  fin = (b0 & 0x80) != 0;

  std::size_t pos = 2;
  unsigned long long len = b1 & 0x7F;
  if (len == 126) {
    if (buf_.size() < 4) return false;
    len = (static_cast<unsigned char>(buf_[2]) << 8) | static_cast<unsigned char>(buf_[3]);
    pos = 4;
  } else if (len == 127) {
    if (buf_.size() < 10) return false;
    len = 0;
    for (int i = 2; i < 10; ++i) len = (len << 8) | static_cast<unsigned char>(buf_[i]);
    pos = 10;
  }

  const bool isControl = (op & 0x8) != 0;
  if (isControl && (len > 125 || !fin)) fail("WebSocket: a bad control frame", 1002);
  if (len > kMaxMessage) fail("WebSocket: frame too large", 1009);
  if (buf_.size() < pos + len) return false;

  payload = buf_.substr(pos, len);
  buf_.erase(0, pos + len);
  opcode = op;
  return true;
}

std::optional<std::string> StreamWebSocket::receiveText(int timeoutMs) {
  if (!open_) throw NetworkError("WebSocket is not connected");

  auto parseBuffered = [&]() -> std::optional<std::string> {
    int opcode = 0;
    bool fin = false;
    std::string payload;
    while (takeFrame(opcode, fin, payload)) {
      switch (opcode) {
        case kText:
          if (inText_ || inBinary_) fail("WebSocket: a new message started before the last one finished", 1002);
          // No size check here: takeFrame already rejected len > kMaxMessage,
          // and partial_ was just cleared, so payload alone can't exceed it.
          partial_ = payload;
          if (fin) {
            std::string out;
            out.swap(partial_);
            return out;
          }
          inText_ = true;
          break;
        case kBinary:
          if (inText_ || inBinary_) fail("WebSocket: a new message started before the last one finished", 1002);
          if (!fin) inBinary_ = true;
          break;  // binary payload ignored
        case kContinuation:
          if (!inText_ && !inBinary_) fail("WebSocket: a continuation frame without a message in progress", 1002);
          if (inText_) {
            if (partial_.size() + payload.size() > kMaxMessage) fail("WebSocket: message too large", 1009);
            partial_ += payload;
            if (fin) {
              inText_ = false;
              std::string out;
              out.swap(partial_);
              return out;
            }
          } else if (fin) {
            inBinary_ = false;
          }
          break;
        case kPing:
          sendFrame(kPong, payload);
          break;
        case kPong:
          break;  // ignored
        case kClose: {
          int code = 0;
          std::string reason;
          if (payload.size() >= 2) {
            code = (static_cast<unsigned char>(payload[0]) << 8) | static_cast<unsigned char>(payload[1]);
            reason = sanitizeForMessage(payload.substr(2));
          }
          std::string msg = "WebSocket closed by the server";
          if (code != 0) msg += " (" + std::to_string(code) + (reason.empty() ? "" : " " + reason) + ")";
          try {
            sendFrame(kClose, "");
          } catch (const NetworkError&) {
          }
          fail(msg, 0);
        }
      }
    }
    return std::nullopt;
  };

  try {
    if (auto msg = parseBuffered()) return msg;
    char chunk[4096];
    const std::size_t n = stream_.readSome(chunk, sizeof chunk, timeoutMs);
    if (n == 0) return std::nullopt;
    buf_.append(chunk, n);
    return parseBuffered();
  } catch (const NetworkError&) {
    resetState();
    throw;
  }
}

void StreamWebSocket::close() {
  if (open_) {
    try {
      sendFrame(kClose, "");
    } catch (const NetworkError&) {
    }
  }
  resetState();
  stream_.close();
}

}  // namespace twitch
