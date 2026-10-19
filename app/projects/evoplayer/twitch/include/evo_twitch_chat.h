#pragma once
// evo_twitch_chat.h - Twitch chat for the channel that is playing.
//
// One worker thread keeps a read-only (anonymous) chat connection open to
// wss://irc-ws.chat.twitch.tv for whichever channel chat_frame() names, and
// fetches that channel's badge pictures. chat_frame() moves what arrived into
// the log on the main thread, so the log never changes while a screen draws it.
//
// Drops: the worker reconnects after 1, 2, 4 ... 30 s and says so in the log
// ("Chat dropped. Trying again in 4 s"); a connection that got as far as the
// JOIN starts that over at 1. A RECONNECT from Twitch after the JOIN went
// through reconnects at once. A connection that dies without a word (Wi-Fi
// gone, a router that forgot it, rest mode) counts as a drop too: the worker
// sends its own PING every 60 s, and 150 s without a single frame from Twitch
// ends it. A JOIN that Twitch hasn't confirmed after 20 s (a channel that
// doesn't exist or is banned) waits the same way, with "This chat isn't
// answering. Trying again in 4 s".
//
// Third-party emotes. Once the JOIN has gone through and Twitch's ROOMSTATE
// has named the channel's user id, the worker fetches the 7TV, BetterTTV and
// FrankerFaceZ emote lists, global and the channel's, one list per 250 ms
// poll so chat keeps coming in between. The global lists are kept for the
// rest of the session. A list that fails is tried once more after the next
// JOIN. From then on a chat line's words that name one of them are pictures.
// Lines that came before the lists stay as they were. Turned off
// (chat_frame()'s thirdPartyEmotes), nothing is fetched and no line gets them.
//
// Chat modes. Twitch's ROOMSTATEs keep the log's room() (slow mode,
// subscribers-only, ...); a change after the first one is a status line.
//
// Threads: everything here is main thread only; the worker is internal.

#include <string>

#include "twitch/badges.hpp"
#include "twitch/chat_log.hpp"
#include "twitch/third_party_emotes.hpp"

namespace evo_twitch {

enum class ChatState { Off, Connecting, Live, Retrying };

struct ChatView {
  const twitch::ChatLog* log = nullptr;          // never null after chat_start()
  const twitch::BadgeCatalog* badges = nullptr;  // never null after chat_start(); empty until
                                                 // the pictures' list arrives
  const twitch::EmoteMap* emotes = nullptr;      // never null after chat_start(); the channel's
                                                 // third-party emotes, empty until they arrive
  ChatState state = ChatState::Off;
  std::string channel;  // the login chat is for, "" when off
};

// Spawns the worker once; later calls do nothing. Call after data_start().
// If the self-test failed, or the worker can't run the core, a channel gets
// the line "Chat isn't available in this build" and stays Off.
void chat_start() noexcept;

// Once per frame, after data_frame(). `channel` is the Twitch login playing
// now, or nullptr / "" when no Twitch stream plays. A different channel
// empties the log and connects to the new one; "" disconnects. Then it moves
// whatever the worker received into the log. Upper case is fine; a name that
// can't be a Twitch login counts as "". thirdPartyEmotes is the setting of
// that name: off, no third-party list is fetched and new lines don't get
// their emotes.
void chat_frame(const char* channel, bool thirdPartyEmotes = true) noexcept;

// The same object for the life of the app. Its contents change only inside
// chat_start() and chat_frame().
const ChatView& chat_view() noexcept;

// Why a message to channel (a login, upper case is fine) may not have gone
// out, when it is the channel chat is following and one of its modes is on:
// twitch::roomStateSendReason(), to follow "Message not sent: ". False when
// there is nothing to say. For a send that Twitch dropped without a reason of
// its own. Main thread only.
bool chat_room_explains(const std::string& channel, std::string* out) noexcept;

// ---- Sending (evo_twitch_chat_send.cpp) ----
//
// Messages go out as the logged-in user through Helix (POST /chat/messages,
// over verified TLS), not through the read-only chat connection. A thread of
// their own sends them, one at a time and oldest first; it starts with the
// first message. Twitch echoes a message that went through back over the chat
// connection like anyone else's, so success shows as the line itself and
// nothing else. A failure leaves a notice for a toast: "Message not sent:"
// and Twitch's own reason when it dropped the message, or ours ("no
// connection", "log in under Settings to chat", ...). A 401 for the token is
// reported to the login service and the message tried once more after the
// refresh; nothing else is retried, since a message that may have arrived
// must not arrive twice. Message texts never reach the log.
//
// The text of a message that wasn't sent is kept for a while
// (chat_peek_draft()), so the keyboard can start with it instead of the user
// typing it again; chat_take_draft() clears it once they submit, so
// cancelling the keyboard doesn't lose it.

// The most messages waiting or on their way at once.
inline constexpr int kChatSendQueueMax = 5;

// Queues text for channel's chat (a login), sent as the user logged in now
// (auth_view()); a message still waiting when someone else logs in isn't
// sent ("Message not sent: you switched Twitch accounts"). Line breaks and
// tabs become spaces. replyId, when not empty, is Twitch's id of the line
// this message answers (Helix reply_parent_message_id). Returns nullptr when
// it was queued, and also for a blank message, which is simply not sent.
// Otherwise the toast saying why not: "Message not sent: log in under
// Settings to chat", "Message not sent: wait for the last ones to go out"
// (already kChatSendQueueMax), "Message not sent: sending isn't available in
// this build". Main thread only.
const char* chat_send(const std::string& channel, const std::string& text,
                      const std::string& replyId = {}) noexcept;

// The notice of a message that failed, oldest first: moves it to *out and
// returns true. A notice the same as the one waiting before it isn't kept
// twice. Main thread only; cheap when there is none.
bool chat_take_send_notice(std::string* out) noexcept;

// How long the text of a message that wasn't sent is kept: 2 minutes.
inline constexpr unsigned long long kChatDraftKeepMs = 2ull * 60ull * 1000ull;

// The text of the last message that wasn't sent, whether chat_send() refused
// it or it failed on its way (a blank one never counts), when it was for
// channel (upper case is fine) and went wrong less than kChatDraftKeepMs ago
// on data_now_ms()'s clock: copies it to *out and returns true, without
// taking it (chat_take_draft(), below, does that). A draft for another
// channel stays for that one; an older one is dropped. Main thread only.
bool chat_peek_draft(const std::string& channel, std::string* out) noexcept;

// The same match as chat_peek_draft(), but takes it: moves it to *out and
// returns true, so it is gone once the user has had another try at it
// (opening the keyboard peeks it for a prefill; this is called once they
// actually submit, so cancelling leaves it for the next time the keyboard
// opens). Main thread only.
bool chat_take_draft(const std::string& channel, std::string* out) noexcept;

}  // namespace evo_twitch
