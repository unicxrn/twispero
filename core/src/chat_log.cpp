#include "twitch/chat_log.hpp"

#include <algorithm>
#include <cstdio>
#include <string_view>
#include <utility>

#include "twitch/third_party_emotes.hpp"

namespace twitch {

namespace {

// Twitch's colours for users who never picked one.
const char* const kDefaultColors[] = {"#FF0000", "#0000FF", "#008000", "#B22222", "#FF7F50",
                                      "#9ACD32", "#FF4500", "#2E8B57", "#DAA520", "#D2691E",
                                      "#5F9EA0", "#1E90FF", "#FF69B4", "#8A2BE2", "#00FF7F"};

int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool parseColor(const std::string& s, int rgb[3]) {
  if (s.size() != 7 || s[0] != '#') return false;
  for (int i = 0; i < 3; ++i) {
    const int hi = hexDigit(s[1 + 2 * i]);
    const int lo = hexDigit(s[2 + 2 * i]);
    if (hi < 0 || lo < 0) return false;
    rgb[i] = hi * 16 + lo;
  }
  return true;
}

char lowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool sameIgnoringCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i)
    if (lowerAscii(a[i]) != lowerAscii(b[i])) return false;
  return true;
}

// A character of a Twitch login: anything else ends a word for mentions().
bool loginChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

std::string plural(int n, const char* one, const char* many) {
  return std::to_string(n) + " " + (n == 1 ? one : many);
}

// "1 minute", "2 hours", "3 days", "1 week": as Twitch's own choices read.
std::string minutesText(int m) {
  if (m > 0 && m % 10080 == 0) return plural(m / 10080, "week", "weeks");
  if (m > 0 && m % 1440 == 0) return plural(m / 1440, "day", "days");
  if (m > 0 && m % 60 == 0) return plural(m / 60, "hour", "hours");
  return plural(m, "minute", "minutes");
}

// "30 seconds", "2 minutes": words, so a line that wraps never leaves a lone "s".
std::string secondsText(int s) {
  if (s >= 60 && s % 60 == 0) return plural(s / 60, "minute", "minutes");
  return plural(s, "second", "seconds");
}

// The status lines for what changed from `was` to `now`, in a fixed order.
std::vector<std::string> roomChanges(const RoomState& was, const RoomState& now) {
  std::vector<std::string> out;
  if (now.emoteOnly != was.emoteOnly) out.push_back(now.emoteOnly ? "Emote-only chat" : "Emote-only chat is off");
  if (now.followersOnly != was.followersOnly) {
    if (now.followersOnly < 0)
      out.push_back("Followers-only chat is off");
    else if (now.followersOnly == 0)
      out.push_back("Followers-only chat");
    else
      out.push_back("Followers-only chat: follow for " + minutesText(now.followersOnly) + " first");
  }
  if (now.slowSeconds != was.slowSeconds)
    out.push_back(now.slowSeconds > 0 ? "Slow mode: one message every " + secondsText(now.slowSeconds)
                                      : std::string("Slow mode is off"));
  if (now.subsOnly != was.subsOnly)
    out.push_back(now.subsOnly ? "Subscribers-only chat" : "Subscribers-only chat is off");
  if (now.uniqueChat != was.uniqueChat)
    out.push_back(now.uniqueChat ? "Unique chat: no repeated messages" : "Unique chat is off");
  return out;
}

// The status lines for a channel's first ROOMSTATE: only the modes that hold
// messages back (emote-only, followers-only, slow, subscribers-only), so a
// viewer who joins one learns it before typing. Nothing when none is on.
std::vector<std::string> roomOnJoin(const RoomState& now) {
  RoomState open;
  open.uniqueChat = now.uniqueChat;  // not one of them
  return roomChanges(open, now);
}

}  // namespace

std::string roomStateSendReason(const RoomState& r) {
  if (!r.known) return {};
  if (r.subsOnly) return "this chat is for subscribers only";
  if (r.followersOnly == 0) return "this chat is for followers only";
  if (r.followersOnly > 0) return "this chat is for followers of " + minutesText(r.followersOnly) + " or more";
  if (r.emoteOnly) return "this chat is emote-only";
  if (r.slowSeconds > 0) return "slow mode, one message every " + secondsText(r.slowSeconds);
  if (r.uniqueChat) return "unique chat is on, so a message can't repeat";
  return {};
}

bool mentions(const ChatLine& line, const std::string& login) {
  if (login.empty() || line.status || sameIgnoringCase(line.user, login)) return false;
  if (sameIgnoringCase(line.replyToLogin, login)) return true;
  for (const ChatSegment& seg : line.segments) {
    if (seg.emote) continue;
    const std::string& t = seg.text;
    for (std::size_t i = 0; i < t.size();) {
      if (!loginChar(t[i])) {
        ++i;
        continue;
      }
      std::size_t j = i;
      while (j < t.size() && loginChar(t[j])) ++j;
      if (sameIgnoringCase(std::string_view(t).substr(i, j - i), login)) return true;
      i = j;
    }
  }
  return false;
}

std::vector<ChatSegment> splitSegments(const ChatMessage& message, const EmoteMap* extra) {
  const std::string& text = message.text;
  // Byte offset of every code point, plus the end. Byte 0 always starts a code
  // point, even a stray continuation byte, so no leading byte is ever lost.
  std::vector<std::size_t> at;
  for (std::size_t i = 0; i < text.size(); ++i)
    if (i == 0 || (static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) at.push_back(i);
  const int count = static_cast<int>(at.size());
  at.push_back(text.size());

  std::vector<EmoteRange> ranges = message.emotes;
  std::sort(ranges.begin(), ranges.end(), [](const EmoteRange& a, const EmoteRange& b) {
    return a.start < b.start || (a.start == b.start && a.end < b.end);
  });

  std::vector<ChatSegment> out;
  const bool lookUp = extra && !extra->empty();
  // Text between Twitch's emotes: as it is, or cut around the words that name
  // a third-party emote.
  auto addText = [&](std::size_t from, std::size_t to) {
    std::size_t done = from;  // the text before this is in out
    if (lookUp) {
      for (std::size_t i = from; i < to;) {
        if (text[i] == ' ') {
          ++i;
          continue;
        }
        std::size_t j = i;
        while (j < to && text[j] != ' ') ++j;
        if (const std::string* url = extra->find(std::string_view(text).substr(i, j - i))) {
          if (i > done) out.push_back(ChatSegment{false, text.substr(done, i - done), "", ""});
          out.push_back(ChatSegment{true, text.substr(i, j - i), "", *url});
          done = j;
        }
        i = j;
      }
    }
    if (to > done) out.push_back(ChatSegment{false, text.substr(done, to - done), "", ""});
  };
  int next = 0;  // first code point not yet emitted
  for (const EmoteRange& r : ranges) {
    if (r.start < next || r.start > r.end || r.end >= count) continue;
    addText(at[next], at[r.start]);
    out.push_back(ChatSegment{true, text.substr(at[r.start], at[r.end + 1] - at[r.start]), r.id, ""});
    next = r.end + 1;
  }
  addText(at[next], text.size());
  return out;
}

std::string nameColor(const std::string& color, const std::string& login) {
  int rgb[3] = {0, 0, 0};
  if (!parseColor(color, rgb)) {
    std::uint32_t h = 2166136261u;  // FNV-1a over the login
    for (unsigned char c : login) {
      h ^= c;
      h *= 16777619u;
    }
    parseColor(kDefaultColors[h % (sizeof kDefaultColors / sizeof kDefaultColors[0])], rgb);
  }
  const double l = (0.299 * rgb[0] + 0.587 * rgb[1] + 0.114 * rgb[2]) / 255.0;
  if (l < 0.5) {
    const double t = (0.5 - l) / (1.0 - l);
    for (int& c : rgb) c = static_cast<int>(c + (255 - c) * t + 0.5);
  }
  char out[8];
  std::snprintf(out, sizeof out, "#%02X%02X%02X", rgb[0], rgb[1], rgb[2]);
  return out;
}

std::string emoteImageUrl(const std::string& emoteId, int scale) {
  const int s = scale < 1 ? 1 : (scale > 3 ? 3 : scale);
  return "https://static-cdn.jtvnw.net/emoticons/v2/" + emoteId + "/static/dark/" + std::to_string(s) + ".0";
}

ChatLog::ChatLog(std::size_t capacity) : capacity_(capacity ? capacity : 1) {}

void ChatLog::push(ChatLine line) {
  line.serial = ++serial_;
  lines_.push_back(std::move(line));
  while (lines_.size() > capacity_) lines_.pop_front();
  ++revision_;
}

template <class Pred>
void ChatLog::removeIf(Pred pred) {
  const std::size_t before = lines_.size();
  lines_.erase(std::remove_if(lines_.begin(), lines_.end(), pred), lines_.end());
  if (lines_.size() != before) ++revision_;
}

void ChatLog::apply(const ChatEvent& e, const EmoteMap* extra) {
  switch (e.kind) {
    case ChatEventKind::Message:
    case ChatEventKind::Notice: {
      ChatLine line;
      line.id = e.message.id;
      line.user = e.message.user;
      line.name = e.message.displayName;
      line.color = nameColor(e.message.color, e.message.user);
      line.badges = e.message.badges;
      line.segments = splitSegments(e.message, extra);
      line.action = e.message.action;
      line.notice = e.kind == ChatEventKind::Notice;
      line.systemText = e.message.systemText;
      line.replyTo = e.message.replyTo;
      line.replyToLogin = e.message.replyToLogin;
      push(std::move(line));
      break;
    }
    case ChatEventKind::RoomState: {
      const RoomStateUpdate& u = e.room;
      RoomState now = room_;
      now.known = true;
      if (!u.roomId.empty()) now.roomId = u.roomId;
      if (u.emoteOnly) now.emoteOnly = *u.emoteOnly;
      if (u.followersOnly) now.followersOnly = *u.followersOnly;
      if (u.slowSeconds) now.slowSeconds = *u.slowSeconds;
      if (u.subsOnly) now.subsOnly = *u.subsOnly;
      if (u.uniqueChat) now.uniqueChat = *u.uniqueChat;
      // The first one says only the modes that are on and hold messages back.
      const std::vector<std::string> said = room_.known ? roomChanges(room_, now) : roomOnJoin(now);
      room_ = std::move(now);
      for (const std::string& text : said) addStatus(text);
      break;
    }
    case ChatEventKind::DeleteMessage:
      if (!e.target.empty())
        removeIf([&](const ChatLine& l) { return !l.status && l.id == e.target; });
      break;
    case ChatEventKind::ClearUser:
      if (!e.target.empty())
        removeIf([&](const ChatLine& l) { return !l.status && l.user == e.target; });
      break;
    case ChatEventKind::ClearChat:
      removeIf([](const ChatLine& l) { return !l.status; });
      addStatus("A moderator cleared the chat");
      break;
    default:
      break;
  }
}

void ChatLog::addStatus(const std::string& text) {
  ChatLine line;
  line.status = true;
  line.systemText = text;
  push(std::move(line));
}

void ChatLog::clear() {
  room_ = RoomState();
  if (lines_.empty()) return;
  lines_.clear();
  ++revision_;
}

}  // namespace twitch
