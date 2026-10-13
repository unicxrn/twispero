#pragma once
#include <string>
#include <vector>

namespace twitch {

// Go-live notifications: which followed channels went live between two looks
// at the followed live list.
//
// The logins in `now` (in its order: the list's, most viewers first) that
// weren't in `before` (logins compare in any case), leaving out `playing`,
// the channel playing now (none when ""), and repeats, at most max of them.
std::vector<std::string> newlyLive(const std::vector<std::string>& before, const std::vector<std::string>& now,
                                   const std::string& playing, int max);

// What the note says: "<name> is live: <title>", the title cut after about
// kGoLiveTitleChars characters (with an ellipsis), or "<name> is live" when
// it has none. name and title are UTF-8 and used as they are (make them safe
// for the fonts first).
inline constexpr int kGoLiveTitleChars = 80;
std::string goLiveText(const std::string& name, const std::string& title);

}  // namespace twitch
