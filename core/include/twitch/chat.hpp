#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "twitch/irc.hpp"
#include "twitch/websocket.hpp"

namespace twitch {

inline constexpr const char* kChatUrl = "wss://irc-ws.chat.twitch.tv";

class ChatClient {
 public:
  explicit ChatClient(WebSocket& ws);

  // Connects read-only as "justinfan<nickNumber>" and requests tags + commands.
  // url is Twitch's chat server; tests pass their own.
  void connectAnonymous(unsigned nickNumber, const std::string& url = kChatUrl);
  // Throws NotFoundError for an invalid channel name (nothing is sent).
  void join(const std::string& channel);  // channel without '#'

  // Waits up to timeoutMs for traffic. Answers PING itself.
  // Returns the chat messages that arrived (often empty).
  std::vector<ChatMessage> poll(int timeoutMs);

  // Like poll(), but every event toChatEvent knows.
  std::vector<ChatEvent> pollEvents(int timeoutMs);

  // How many frames poll()/pollEvents() have received, whatever they held (a
  // PING, a PONG, lines it ignores). A count that stops moving while our own
  // PINGs go out means the connection died without saying so.
  std::uint64_t framesReceived() const { return frames_; }

 private:
  WebSocket& ws_;
  std::uint64_t frames_ = 0;
};

}  // namespace twitch
