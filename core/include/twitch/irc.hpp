#pragma once
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace twitch {

struct IrcMessage {
  std::map<std::string, std::string> tags;  // values already unescaped
  std::string prefix;                       // "nick!user@host" or "tmi.twitch.tv"
  std::string command;                      // "PRIVMSG", "PING", "001", ...
  std::vector<std::string> params;          // trailing parameter (after " :") is the last entry
};

struct EmoteRange {
  std::string id;
  int start = 0;  // inclusive, in Unicode code points of the message text (NOT bytes; text is UTF-8)
  int end = 0;    // inclusive; toChatMessage guarantees 0 <= start <= end < code point count of text
};

struct Badge {
  std::string set;      // "moderator", "subscriber", ...
  std::string version;  // picture version, e.g. "12" for a 12-month sub badge
};

struct ChatMessage {
  std::string id;
  std::string channel;      // without '#'
  std::string user;         // login
  std::string displayName;  // falls back to user when the tag is empty
  std::string color;        // "#FF69B4" or "" when the user never set one
  std::string text;         // UTF-8; emote ranges index code points, convert before slicing bytes
  std::vector<Badge> badges;  // in tag order
  std::vector<EmoteRange> emotes;
  bool action = false;  // "/me" message; text has the CTCP wrapper removed
  std::string systemText;  // USERNOTICE: Twitch's own sentence ("Amy subscribed for 6 months!"); "" otherwise
  // A reply: who it answers, by display name (their login when Twitch left
  // that out) and login; both "" otherwise. Twitch puts "@<their login> " in
  // front of a reply's text; toChatMessage drops it (the emote ranges move
  // with the text), since the reply line already names them.
  std::string replyTo;
  std::string replyToLogin;
};

// One ROOMSTATE: the chat's modes it names. On JOIN Twitch sends them all;
// later ones carry only what changed, so each mode is unset when not named.
struct RoomStateUpdate {
  std::string roomId;                // the channel's Twitch user id (digits), "" when not named
  std::optional<bool> emoteOnly;
  std::optional<int> followersOnly;  // minutes a viewer must have followed; -1 off, 0 any follower
  std::optional<int> slowSeconds;    // between one viewer's messages; 0 off
  std::optional<bool> subsOnly;
  std::optional<bool> uniqueChat;    // "r9k": a message may not repeat a recent one
};

// Parses one IRC line (without the trailing \r\n).
IrcMessage parseIrcLine(const std::string& line);

// IRCv3 tag unescaping: \s -> space, \: -> ;, \\ -> \, \r and \n.
std::string unescapeTagValue(const std::string& value);

// Only PRIVMSG becomes a ChatMessage; everything else returns nullopt.
std::optional<ChatMessage> toChatMessage(const IrcMessage& msg);

enum class ChatEventKind {
  Message,        // PRIVMSG: `message`
  Notice,         // USERNOTICE (sub, raid, ...): `message`, with systemText; text may be ""
  DeleteMessage,  // CLEARMSG: `target` is the message id
  ClearUser,      // CLEARCHAT with a user: `target` is the login (timeout or ban)
  ClearChat,      // CLEARCHAT without a user: a moderator cleared the chat
  Reconnect,      // RECONNECT: Twitch asks the client to reconnect
  Joined,         // 366 (end of NAMES): the JOIN went through
  RoomState,      // ROOMSTATE: `room`
};

struct ChatEvent {
  ChatEventKind kind = ChatEventKind::Message;
  ChatMessage message;  // Message and Notice
  std::string target;   // DeleteMessage and ClearUser
  RoomStateUpdate room;  // RoomState
};

// Everything chat cares about; nullopt for the rest (PING, numerics other than 366, ...).
std::optional<ChatEvent> toChatEvent(const IrcMessage& msg);

}  // namespace twitch
