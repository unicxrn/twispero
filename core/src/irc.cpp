#include "twitch/irc.hpp"

#include <cerrno>
#include <climits>
#include <initializer_list>
#include <cstdlib>
#include <utility>

namespace twitch {
namespace {

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  if (s.empty()) return out;
  size_t start = 0;
  while (true) {
    size_t pos = s.find(sep, start);
    out.push_back(s.substr(start, pos - start));
    if (pos == std::string::npos) break;
    start = pos + 1;
  }
  return out;
}

std::string tagOr(const IrcMessage& m, const char* key) {
  auto it = m.tags.find(key);
  return it == m.tags.end() ? std::string() : it->second;
}

// Parses a non-negative decimal int; returns -1 for empty, non-numeric or out-of-range text.
int parseIndex(const std::string& s) {
  if (s.empty()) return -1;
  char* end = nullptr;
  errno = 0;
  long v = std::strtol(s.c_str(), &end, 10);
  if (*end != '\0' || errno == ERANGE || v < 0 || v > INT_MAX) return -1;
  return static_cast<int>(v);
}

// Number of Unicode code points in UTF-8 text (counts every byte that is not a continuation byte).
int codePointCount(const std::string& s) {
  int n = 0;
  for (unsigned char c : s)
    if ((c & 0xC0) != 0x80) ++n;
  return n;
}

char lowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

// How many bytes of a reply's text are the "@<name> " Twitch put in front of
// it: name is the parent's login or display name, in any case. 0 when the text
// doesn't start that way, or when nothing would be left after it.
size_t replyPrefix(const std::string& text, const std::string& login, const std::string& displayName) {
  if (text.empty() || text[0] != '@') return 0;
  for (const std::string* name : {&login, &displayName}) {
    const size_t n = name->size();
    if (n == 0 || text.size() <= n + 2 || text[n + 1] != ' ') continue;
    bool same = true;
    for (size_t i = 0; i < n && same; ++i) same = lowerAscii(text[1 + i]) == lowerAscii((*name)[i]);
    if (same) return n + 2;
  }
  return 0;
}

// Shared by toChatMessage (PRIVMSG) and toChatEvent (USERNOTICE): id, channel, user, display name,
// colour, text, the "/me" handling, a reply's parent, badges and emotes.
ChatMessage fromTags(const IrcMessage& m, std::string text) {
  ChatMessage c;
  c.id = tagOr(m, "id");
  c.channel = m.params[0].rfind('#', 0) == 0 ? m.params[0].substr(1) : m.params[0];
  std::string login = tagOr(m, "login");
  c.user = !login.empty() ? login : m.prefix.substr(0, m.prefix.find('!'));
  c.displayName = tagOr(m, "display-name");
  if (c.displayName.empty()) c.displayName = c.user;
  c.color = tagOr(m, "color");
  c.text = std::move(text);

  // CTCP ACTION wrapper for "/me" messages: "\x01ACTION <text>\x01".
  static const std::string kActionPrefix = "\x01" "ACTION ";
  if (c.text.size() >= kActionPrefix.size() + 1 && c.text.compare(0, kActionPrefix.size(), kActionPrefix) == 0 &&
      c.text.back() == '\x01') {
    c.action = true;
    c.text = c.text.substr(kActionPrefix.size(), c.text.size() - kActionPrefix.size() - 1);
  }

  for (const auto& badge : split(tagOr(m, "badges"), ',')) {
    if (badge.empty()) continue;
    size_t slash = badge.find('/');
    c.badges.push_back(Badge{badge.substr(0, slash), slash == std::string::npos ? "" : badge.substr(slash + 1)});
  }

  c.replyToLogin = tagOr(m, "reply-parent-user-login");
  c.replyTo = tagOr(m, "reply-parent-display-name");
  if (c.replyTo.empty()) c.replyTo = c.replyToLogin;
  int shift = 0;  // code points dropped from the front of the text
  if (const size_t cut = c.replyTo.empty() ? 0 : replyPrefix(c.text, c.replyToLogin, c.replyTo)) {
    shift = codePointCount(c.text.substr(0, cut));
    c.text.erase(0, cut);
  }

  int textLength = codePointCount(c.text);

  // "25:0-4,12-16/1902:6-10", counted in the text as Twitch sent it
  for (const auto& group : split(tagOr(m, "emotes"), '/')) {
    size_t colon = group.find(':');
    if (colon == std::string::npos || colon == 0) continue;
    std::string id = group.substr(0, colon);
    for (const auto& range : split(group.substr(colon + 1), ',')) {
      size_t dash = range.find('-');
      if (dash == std::string::npos) continue;
      int start = parseIndex(range.substr(0, dash));
      int end = parseIndex(range.substr(dash + 1));
      if (start < shift || end < start) continue;  // a bad range, or one in the dropped "@name "
      start -= shift;
      end -= shift;
      if (end >= textLength) continue;
      c.emotes.push_back(EmoteRange{id, start, end});
    }
  }
  return c;
}

// A ROOMSTATE flag ("0" or "1"), when the tag is there and readable.
void readFlag(const IrcMessage& m, const char* key, std::optional<bool>& out) {
  const auto it = m.tags.find(key);
  if (it == m.tags.end()) return;
  if (it->second == "0") out = false;
  else if (it->second == "1") out = true;
}

// A ROOMSTATE number of at least `lowest` (-1 or 0), when the tag is there and readable.
void readNumber(const IrcMessage& m, const char* key, int lowest, std::optional<int>& out) {
  const auto it = m.tags.find(key);
  if (it == m.tags.end()) return;
  if (lowest < 0 && it->second == "-1") {
    out = -1;
    return;
  }
  const int v = parseIndex(it->second);
  if (v >= 0) out = v;
}

bool allDigits(const std::string& s) {
  if (s.empty()) return false;
  for (const char c : s)
    if (c < '0' || c > '9') return false;
  return true;
}

}  // namespace

std::string unescapeTagValue(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (size_t i = 0; i < value.size(); ++i) {
    if (value[i] != '\\') {
      out.push_back(value[i]);
      continue;
    }
    if (i + 1 >= value.size()) break;  // a trailing lone backslash is dropped (IRCv3)
    char next = value[++i];
    switch (next) {
      case 's': out.push_back(' '); break;
      case ':': out.push_back(';'); break;
      case '\\': out.push_back('\\'); break;
      case 'r': out.push_back('\r'); break;
      case 'n': out.push_back('\n'); break;
      default: out.push_back(next); break;
    }
  }
  return out;
}

IrcMessage parseIrcLine(const std::string& line) {
  IrcMessage m;
  size_t pos = 0;

  if (pos < line.size() && line[pos] == '@') {
    size_t end = line.find(' ', pos);
    for (const auto& tag : split(line.substr(pos + 1, end == std::string::npos ? std::string::npos : end - pos - 1), ';')) {
      size_t eq = tag.find('=');
      if (eq == std::string::npos)
        m.tags[tag] = "";
      else
        m.tags[tag.substr(0, eq)] = unescapeTagValue(tag.substr(eq + 1));
    }
    pos = end == std::string::npos ? line.size() : end + 1;
  }

  if (pos < line.size() && line[pos] == ':') {
    size_t end = line.find(' ', pos);
    m.prefix = line.substr(pos + 1, end == std::string::npos ? std::string::npos : end - pos - 1);
    pos = end == std::string::npos ? line.size() : end + 1;
  }

  if (pos >= line.size()) return m;

  // Skip repeated spaces before command
  while (pos < line.size() && line[pos] == ' ') ++pos;
  if (pos >= line.size()) return m;

  size_t end = line.find(' ', pos);
  m.command = line.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
  pos = end == std::string::npos ? line.size() : end + 1;

  while (pos < line.size()) {
    // Skip repeated spaces before param
    while (pos < line.size() && line[pos] == ' ') ++pos;
    if (pos >= line.size()) break;

    if (line[pos] == ':') {
      m.params.push_back(line.substr(pos + 1));
      break;
    }
    end = line.find(' ', pos);
    m.params.push_back(line.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
    if (end == std::string::npos) break;
    pos = end + 1;
  }
  return m;
}

std::optional<ChatMessage> toChatMessage(const IrcMessage& m) {
  if (m.command != "PRIVMSG" || m.params.size() < 2) return std::nullopt;
  return fromTags(m, m.params[1]);
}

std::optional<ChatEvent> toChatEvent(const IrcMessage& m) {
  if (m.command == "PRIVMSG") {
    auto chat = toChatMessage(m);
    if (!chat) return std::nullopt;
    ChatEvent e;
    e.kind = ChatEventKind::Message;
    e.message = std::move(*chat);
    return e;
  }

  if (m.command == "USERNOTICE") {
    if (m.params.empty()) return std::nullopt;
    ChatEvent e;
    e.kind = ChatEventKind::Notice;
    e.message = fromTags(m, m.params.size() >= 2 ? m.params[1] : std::string());
    e.message.systemText = tagOr(m, "system-msg");
    return e;
  }

  if (m.command == "CLEARMSG") {
    ChatEvent e;
    e.kind = ChatEventKind::DeleteMessage;
    e.target = tagOr(m, "target-msg-id");
    return e;
  }

  if (m.command == "CLEARCHAT") {
    ChatEvent e;
    if (m.params.size() >= 2) {
      e.kind = ChatEventKind::ClearUser;
      e.target = m.params[1];
    } else {
      e.kind = ChatEventKind::ClearChat;
    }
    return e;
  }

  if (m.command == "RECONNECT") {
    ChatEvent e;
    e.kind = ChatEventKind::Reconnect;
    return e;
  }

  if (m.command == "ROOMSTATE") {
    ChatEvent e;
    e.kind = ChatEventKind::RoomState;
    RoomStateUpdate& r = e.room;
    if (const std::string id = tagOr(m, "room-id"); allDigits(id)) r.roomId = id;
    readFlag(m, "emote-only", r.emoteOnly);
    readNumber(m, "followers-only", -1, r.followersOnly);
    readNumber(m, "slow", 0, r.slowSeconds);
    readFlag(m, "subs-only", r.subsOnly);
    readFlag(m, "r9k", r.uniqueChat);
    return e;
  }

  if (m.command == "366") {
    ChatEvent e;
    e.kind = ChatEventKind::Joined;
    return e;
  }

  return std::nullopt;
}

}  // namespace twitch
