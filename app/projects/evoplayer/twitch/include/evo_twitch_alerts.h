#pragma once
// evo_twitch_alerts.h - go-live notifications: a note when a channel the
// logged-in user follows goes live while the app runs.
//
// While someone is logged in and Settings > Twitch > Live notifications is
// on, the followed live list (evo_twitch_data.h's followed_live()) is asked
// for again every 2 minutes, counted from its last answer, whoever asked
// (Home and Following ask too; the data service's own rules for a refresh
// and a request already on its way apply). Each new answer is compared with
// the one before it (twitch::newlyLive): the channels that weren't in it get
// a note, "<name> is live: <title>", at most 3 an answer, most viewers
// first. The first answer after the app starts, after a login or after the
// setting is turned on is only remembered, and the channel playing live now
// gets none. A channel noted once isn't noted again for 30 minutes (a list
// that drops a channel for one answer and has it back on the next would
// otherwise note it twice).
//
// The notes wait their turn: the app shows one as a toast once no other
// toast is up, and drops one that waited over a minute.
//
// Main thread only. Nothing here throws. No core headers here.

#include <string>

namespace evo_twitch {

// Once per frame, right after data_frame() and before auth_frame(), so the
// followed live slot and the login it reads belong together (data_frame()
// empties the slot for a login that changed).
void live_alerts_frame() noexcept;

// The next note to show: moves it to *out and returns true; false when none
// waits.
bool live_alerts_take(std::string* out) noexcept;

// The same note in two parts, as the toast shows it: *headline gets
// "<name> is live" and *detail the stream's title (cut as above; "" when it
// has none).
bool live_alerts_take(std::string* headline, std::string* detail) noexcept;

}  // namespace evo_twitch
