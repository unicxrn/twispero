#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "twitch/irc.hpp"

namespace twitch {

class EmoteMap;  // twitch/third_party_emotes.hpp

// One piece of a chat line: plain text or an emote picture.
struct ChatSegment {
  bool emote = false;
  std::string text;      // the text, or the emote's name as typed (drawn while its picture loads)
  std::string emoteId;   // a Twitch emote's id
  std::string imageUrl;  // a 7TV, BetterTTV or FrankerFaceZ emote (emoteId ""): its picture
};

struct ChatLine {
  std::uint64_t serial = 0;  // one higher for every line added; a key for layout caches
  std::string id;            // Twitch's message id, "" for the app's own lines
  std::string user;          // login, "" for the app's own lines
  std::string name;          // display name
  std::string color;         // "#RRGGBB", readable on a dark background
  std::vector<Badge> badges;
  std::vector<ChatSegment> segments;
  bool action = false;     // "/me": the whole line takes the name's colour
  bool notice = false;     // a sub, raid or other channel event
  bool status = false;     // the app's own line ("Connecting to chat")
  std::string systemText;  // notice: Twitch's sentence about it; status: the line itself
  std::string replyTo;       // a reply: who it answers (display name), "" otherwise
  std::string replyToLogin;  // and their login
};

// The chat's modes, as the ROOMSTATEs so far have left them.
struct RoomState {
  bool known = false;       // a ROOMSTATE has come for this channel
  std::string roomId;       // the channel's Twitch user id
  bool emoteOnly = false;
  int followersOnly = -1;   // minutes a viewer must have followed; -1 off, 0 any follower
  int slowSeconds = 0;      // 0 off
  bool subsOnly = false;
  bool uniqueChat = false;  // a message may not repeat a recent one
};

// Why a message may not have gone out, given these modes: the strictest one
// that is on, worded to follow "Message not sent: " ("this chat is for
// subscribers only"). "" when none is on or nothing is known.
std::string roomStateSendReason(const RoomState& room);

// A message's text as text and emote segments. Emote ranges count code points;
// they are sorted, and overlapping ones are skipped. Empty text pieces are left out.
// With `extra`, a word (between spaces) of the text left over that names an
// emote in it becomes an emote segment too, with its picture in imageUrl.
std::vector<ChatSegment> splitSegments(const ChatMessage& message, const EmoteMap* extra = nullptr);

// The line speaks to `login` (the logged-in user, any case): it is a reply to
// them, or its text has their login as a word, with or without "@". Never for
// their own lines, the app's lines, or an empty login.
bool mentions(const ChatLine& line, const std::string& login);

// The colour to draw a name in: the user's own "#RRGGBB" (upper case), or one
// of Twitch's default colours picked from the login when there is none, then
// lightened until it reads on a dark background.
std::string nameColor(const std::string& color, const std::string& login);

// The CDN address of an emote's static picture; scale 1, 2 or 3 is 28, 56 or 112 px.
std::string emoteImageUrl(const std::string& emoteId, int scale);

// The newest chat lines, oldest first, at most `capacity` of them.
class ChatLog {
 public:
  explicit ChatLog(std::size_t capacity = 150);

  // Message and Notice add a line (with the third-party emotes in `extra`, if
  // given); DeleteMessage, ClearUser and ClearChat take lines away (ClearChat
  // leaves a status line saying so). RoomState updates room(); every mode it
  // changes after the channel's first ROOMSTATE gets a status line ("Slow mode:
  // one message every 30 seconds"). The first one gets lines only for the
  // modes it finds on that hold messages back (emote-only, followers-only,
  // slow, subscribers-only); unique chat and modes that are off go unsaid.
  // The rest is ignored.
  void apply(const ChatEvent& event, const EmoteMap* extra = nullptr);
  void addStatus(const std::string& text);
  // Empties the log and forgets the room's modes: for another channel.
  void clear();

  const std::deque<ChatLine>& lines() const { return lines_; }
  const RoomState& room() const { return room_; }
  // Changes whenever lines() does.
  std::uint64_t revision() const { return revision_; }

 private:
  void push(ChatLine line);
  template <class Pred>
  void removeIf(Pred pred);

  std::size_t capacity_;
  std::deque<ChatLine> lines_;
  RoomState room_;
  std::uint64_t serial_ = 0;
  std::uint64_t revision_ = 0;
};

}  // namespace twitch
