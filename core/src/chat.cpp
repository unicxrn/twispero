#include "twitch/chat.hpp"

#include "twitch/login.hpp"

namespace twitch {

ChatClient::ChatClient(WebSocket& ws) : ws_(ws) {}

void ChatClient::connectAnonymous(unsigned nickNumber, const std::string& url) {
  ws_.connect(url);
  ws_.sendText("CAP REQ :twitch.tv/tags twitch.tv/commands");
  ws_.sendText("PASS SCHMOOPIIE");
  ws_.sendText("NICK justinfan" + std::to_string(nickNumber));
}

void ChatClient::join(const std::string& channel) {
  ws_.sendText("JOIN #" + normalizeLogin(channel));
}

std::vector<ChatEvent> ChatClient::pollEvents(int timeoutMs) {
  std::vector<ChatEvent> out;
  auto frame = ws_.receiveText(timeoutMs);
  if (!frame) return out;
  ++frames_;

  size_t start = 0;
  while (start < frame->size()) {
    size_t end = frame->find("\r\n", start);
    std::string line = frame->substr(start, end == std::string::npos ? std::string::npos : end - start);
    start = end == std::string::npos ? frame->size() : end + 2;
    if (line.empty()) continue;

    IrcMessage msg = parseIrcLine(line);
    if (msg.command == "PING") {
      ws_.sendText("PONG :" + (msg.params.empty() ? std::string("tmi.twitch.tv") : msg.params.back()));
    } else if (auto event = toChatEvent(msg)) {
      out.push_back(std::move(*event));
    }
  }
  return out;
}

std::vector<ChatMessage> ChatClient::poll(int timeoutMs) {
  std::vector<ChatMessage> out;
  for (auto& event : pollEvents(timeoutMs))
    if (event.kind == ChatEventKind::Message) out.push_back(std::move(event.message));
  return out;
}

}  // namespace twitch
