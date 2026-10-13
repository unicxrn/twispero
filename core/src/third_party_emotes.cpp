#include "twitch/third_party_emotes.hpp"

#include <initializer_list>
#include <utility>

#include <nlohmann/json.hpp>

#include "json_helpers.hpp"
#include "twitch/error.hpp"
#include "twitch/http.hpp"

namespace twitch {

namespace {

using nlohmann::json;

// A body this big is no emote list. The largest seen, a 7TV channel with
// about 1,000 emotes, was 2.4 MB.
constexpr std::size_t kMaxBody = 16u * 1024 * 1024;

bool allDigits(const std::string& s) {
  if (s.empty()) return false;
  for (const char c : s)
    if (c < '0' || c > '9') return false;
  return true;
}

// An id that can go into a URL path as it is: letters and digits.
bool goodId(const std::string& s) {
  if (s.empty() || s.size() > 64) return false;
  for (const char c : s)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) return false;
  return true;
}

// A name chat can show as one word: no spaces or control characters.
bool goodName(const std::string& s) {
  if (s.empty() || s.size() > 100) return false;
  for (const char c : s)
    if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7F) return false;
  return true;
}

// "https://..." as it is and "//..." with https in front; "" for anything else.
std::string httpsUrl(const std::string& url) {
  if (url.rfind("https://", 0) == 0) return url;
  if (url.rfind("//", 0) == 0 && url.size() > 2) return "https:" + url;
  return {};
}

bool boolOr(const json* obj, const char* key, bool def) {
  if (!obj || !obj->is_object()) return def;
  const auto it = obj->find(key);
  return it != obj->end() && it->is_boolean() ? it->get<bool>() : def;
}

const json* arrayOr(const json* obj, const char* key) {
  if (!obj || !obj->is_object()) return nullptr;
  const auto it = obj->find(key);
  return it != obj->end() && it->is_array() ? &*it : nullptr;
}

// Parses body keeping only the keys in `keep`, plus keys that are all digits
// (FFZ's set ids and picture sizes). The rest is dropped while it is read, so
// a 7TV channel whose answer is megabytes of picture variants and owners
// costs no more than its names and ids. Throws ApiError when it isn't a JSON
// object or list.
json parseKeeping(const std::string& body, std::initializer_list<const char*> keep, const char* service) {
  if (body.size() > kMaxBody) throw ApiError(std::string(service) + " emotes: the answer is too large");
  const json::parser_callback_t onlyKept = [keep](int, json::parse_event_t event, json& parsed) {
    if (event != json::parse_event_t::key || !parsed.is_string()) return true;
    const std::string& key = parsed.get_ref<const std::string&>();
    if (allDigits(key)) return true;
    for (const char* k : keep)
      if (key == k) return true;
    return false;
  };
  json j = json::parse(body, onlyKept, /*allow_exceptions=*/false);
  if (j.is_discarded() || !(j.is_object() || j.is_array()))
    throw ApiError(std::string(service) + " emotes: the answer is not a JSON object or list");
  return j;
}

void addBttv(std::vector<ThirdPartyEmote>& out, const json* list) {
  if (!list) return;
  for (const json& e : *list) {
    const std::string id = stringOr(&e, "id");
    std::string code = stringOr(&e, "code");
    if (!goodId(id) || !goodName(code) || boolOr(&e, "modifier", false)) continue;
    // An animated emote has a PNG of its first frame at the same address.
    out.push_back({std::move(code), "https://cdn.betterttv.net/emote/" + id + "/2x.png"});
  }
}

void addFfzSet(std::vector<ThirdPartyEmote>& out, const json* set) {
  const json* list = arrayOr(set, "emoticons");
  if (!list) return;
  for (const json& e : *list) {
    std::string name = stringOr(&e, "name");
    if (!goodName(name) || boolOr(&e, "modifier", false)) continue;
    const json* urls = objectOr(&e, "urls");
    std::string url = httpsUrl(stringOr(urls, "2"));  // twice the size; not every emote has it
    if (url.empty()) url = httpsUrl(stringOr(urls, "1"));
    if (url.empty()) continue;
    out.push_back({std::move(name), std::move(url)});
  }
}

const char* serviceName(EmoteProvider p) {
  switch (p) {
    case EmoteProvider::SevenTv: return "7TV";
    case EmoteProvider::Bttv: return "BetterTTV";
    default: return "FrankerFaceZ";
  }
}

}  // namespace

std::string thirdPartyEmotesUrl(EmoteProvider provider, const std::string& twitchUserId) {
  const bool global = twitchUserId.empty();
  switch (provider) {
    case EmoteProvider::SevenTv:
      return global ? "https://7tv.io/v3/emote-sets/global" : "https://7tv.io/v3/users/twitch/" + twitchUserId;
    case EmoteProvider::Bttv:
      return global ? "https://api.betterttv.net/3/cached/emotes/global"
                    : "https://api.betterttv.net/3/cached/users/twitch/" + twitchUserId;
    default:
      return global ? "https://api.frankerfacez.com/v1/set/global"
                    : "https://api.frankerfacez.com/v1/room/id/" + twitchUserId;
  }
}

// 7TV, global: {"emotes": [...]}; a channel: {"emote_set": {"emotes": [...]}}
// (emote_set is null for a user without one). An emote: {"id", "name" (the
// name in this set, which can differ from data.name), "data": {"animated",
// "host": {"url": "//cdn.7tv.app/emote/<id>", "files": [...]}}}. On the CDN a
// still emote has 2x.png (64 px tall); an animated one has no 2x.png, only
// 2x_static.png, its first frame. webp and avif exist for both, but the
// image service can't decode those.
std::vector<ThirdPartyEmote> parseSevenTvEmotes(const std::string& body) {
  const json j = parseKeeping(body, {"emote_set", "emotes", "id", "name", "data", "animated", "host", "url"}, "7TV");
  std::vector<ThirdPartyEmote> out;
  const json* list = nullptr;
  if (j.is_object()) {
    if (j.contains("emote_set"))
      list = arrayOr(objectOr(&j, "emote_set"), "emotes");
    else
      list = arrayOr(&j, "emotes");
  }
  if (!list) return out;
  for (const json& e : *list) {
    const std::string id = stringOr(&e, "id");
    std::string name = stringOr(&e, "name");
    if (!goodId(id) || !goodName(name)) continue;
    const json* data = objectOr(&e, "data");
    std::string base = httpsUrl(stringOr(objectOr(data, "host"), "url"));
    if (base.empty()) base = "https://cdn.7tv.app/emote/" + id;
    out.push_back({std::move(name), base + (boolOr(data, "animated", false) ? "/2x_static.png" : "/2x.png")});
  }
  return out;
}

// BetterTTV, global: a list of {"id", "code", "imageType": "png"|"gif",
// "animated", "modifier"}; a channel: {"channelEmotes": [...], "sharedEmotes":
// [...]} of the same. Pictures: cdn.betterttv.net/emote/<id>/2x.png, 56 px
// tall, a PNG of the first frame for animated ones too. The modifiers ("h!",
// "w!" ...) change the emote after them in BetterTTV's own client and are left
// out.
std::vector<ThirdPartyEmote> parseBttvEmotes(const std::string& body) {
  const json j = parseKeeping(body, {"channelEmotes", "sharedEmotes", "id", "code", "modifier"}, "BetterTTV");
  std::vector<ThirdPartyEmote> out;
  if (j.is_array()) {
    addBttv(out, &j);
  } else {
    addBttv(out, arrayOr(&j, "channelEmotes"));
    addBttv(out, arrayOr(&j, "sharedEmotes"));
  }
  return out;
}

// FrankerFaceZ, global: {"default_sets": [3, ...], "sets": {"3": {"emoticons":
// [...]}, ...}}, where only the default sets are everyone's; a channel:
// {"room": {...}, "sets": {"<id>": {"emoticons": [...]}}}. An emoticon:
// {"name", "modifier", "urls": {"1", "2", "4"}}, PNGs at 1x, 2x and 4x, with
// "2" and "4" only when the picture is big enough.
std::vector<ThirdPartyEmote> parseFfzEmotes(const std::string& body) {
  const json j =
      parseKeeping(body, {"default_sets", "sets", "emoticons", "name", "urls", "modifier"}, "FrankerFaceZ");
  std::vector<ThirdPartyEmote> out;
  const json* sets = objectOr(&j, "sets");
  if (!sets) return out;
  if (const json* defaults = arrayOr(&j, "default_sets")) {
    for (const json& id : *defaults)
      if (id.is_number_integer()) addFfzSet(out, objectOr(sets, std::to_string(id.get<long long>()).c_str()));
  } else {
    for (const auto& set : sets->items()) addFfzSet(out, &set.value());
  }
  return out;
}

std::vector<ThirdPartyEmote> fetchThirdPartyEmotes(HttpClient& http, EmoteProvider provider,
                                                   const std::string& twitchUserId) {
  if (!twitchUserId.empty() && !allDigits(twitchUserId)) throw NotFoundError("not a Twitch user id");
  HttpRequest req;
  req.url = thirdPartyEmotesUrl(provider, twitchUserId);
  const HttpResponse res = http.send(req);
  if (res.status == 404) return {};
  if (res.status < 200 || res.status >= 300)
    throw HttpStatusError(std::string(serviceName(provider)) + " emotes: HTTP " + std::to_string(res.status),
                          res.status);
  switch (provider) {
    case EmoteProvider::SevenTv: return parseSevenTvEmotes(res.body);
    case EmoteProvider::Bttv: return parseBttvEmotes(res.body);
    default: return parseFfzEmotes(res.body);
  }
}

void EmoteMap::add(const std::vector<ThirdPartyEmote>& emotes) {
  for (const ThirdPartyEmote& e : emotes) urls_.insert_or_assign(e.name, e.url);
}

const std::string* EmoteMap::find(std::string_view name) const {
  const auto it = urls_.find(name);
  return it == urls_.end() ? nullptr : &it->second;
}

}  // namespace twitch
