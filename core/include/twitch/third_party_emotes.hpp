#pragma once
#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// Emotes from 7TV, BetterTTV and FrankerFaceZ: public APIs, no login. Each has
// a global list and one per channel, keyed by the channel's Twitch user id.
// What each answers (checked 2026-10-13) is noted at its parser in
// third_party_emotes.cpp. Every picture is a PNG about 56 px tall (an
// animated emote's first frame), which is what the app's image service can
// decode. This header stays clear of the JSON library: the chat log and the
// player's chat include it.

namespace twitch {

class HttpClient;  // twitch/http.hpp

enum class EmoteProvider { SevenTv, Bttv, Ffz };

struct ThirdPartyEmote {
  std::string name;  // as typed in chat, case and all
  std::string url;   // its picture
};

// Where a service keeps its global list (twitchUserId "") or a channel's.
std::string thirdPartyEmotesUrl(EmoteProvider provider, const std::string& twitchUserId);

// The emotes in one of those answers. Entries that are broken, or that are
// modifiers rather than pictures, are left out. A body that isn't JSON (or
// not an object or list) throws ApiError.
std::vector<ThirdPartyEmote> parseSevenTvEmotes(const std::string& body);
std::vector<ThirdPartyEmote> parseBttvEmotes(const std::string& body);
std::vector<ThirdPartyEmote> parseFfzEmotes(const std::string& body);

// GETs a list and parses it. A 404 (a channel the service doesn't know) is an
// empty list; another status throws HttpStatusError, no answer NetworkError.
// A user id that isn't all digits throws NotFoundError without asking.
std::vector<ThirdPartyEmote> fetchThirdPartyEmotes(HttpClient& http, EmoteProvider provider,
                                                   const std::string& twitchUserId);

// Emote names to pictures, for splitSegments.
class EmoteMap {
 public:
  // A name already there takes the newer picture, so add the global lists
  // first and the channel's last.
  void add(const std::vector<ThirdPartyEmote>& emotes);
  // The picture for exactly this name, nullptr when there is none.
  const std::string* find(std::string_view name) const;
  bool empty() const { return urls_.empty(); }
  std::size_t size() const { return urls_.size(); }

 private:
  std::map<std::string, std::string, std::less<>> urls_;
};

}  // namespace twitch
