#include "twitch/playback.hpp"

#include <algorithm>
#include <cmath>

#include "twitch/error.hpp"
#include "twitch/login.hpp"
#include "twitch/url.hpp"

namespace twitch {
namespace {

// "1080p60 (source)" -> "1080p60"; labels without the suffix are returned unchanged.
std::string stripSourceSuffix(const std::string& label) {
  static const std::string kSuffix = " (source)";
  if (label.size() >= kSuffix.size() &&
      label.compare(label.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0) {
    return label.substr(0, label.size() - kSuffix.size());
  }
  return label;
}

// Twitch streams at two rates: 60 for anything made for 50/60 fps playback, 30
// for the rest (and when the master doesn't say).
int rateClass(double fps) { return fps >= 45.0 ? 60 : 30; }

// "720p60" -> 720 and 60, "480p" -> 480 and 30 (Twitch's names leave 30 out).
// False for anything that isn't a height, "p" and maybe a frame rate.
bool parseQualityName(const std::string& name, int* height, int* rate) {
  size_t i = 0;
  int h = 0;
  for (; i < name.size() && i < 5 && name[i] >= '0' && name[i] <= '9'; ++i) h = h * 10 + (name[i] - '0');
  if (i == 0 || h <= 0 || i >= name.size() || name[i] != 'p') return false;
  ++i;
  int f = 0;
  size_t digits = 0;
  for (; i < name.size() && digits < 3 && name[i] >= '0' && name[i] <= '9'; ++i, ++digits)
    f = f * 10 + (name[i] - '0');
  if (i != name.size()) return false;
  *height = h;
  *rate = digits ? rateClass(f) : 30;
  return true;
}

// a is the better quality: taller, then faster, then more bandwidth.
bool ranksAbove(const Variant& a, const Variant& b) {
  if (a.height != b.height) return a.height > b.height;
  if (a.fps != b.fps) return a.fps > b.fps;
  return a.bandwidth > b.bandwidth;
}

bool isAudioOnly(const Variant& v) { return v.name == "audio_only" || v.label == "audio_only"; }

}  // namespace

const char* const kPlaybackTokenQuery =
    "query PlaybackAccessToken($login: String!, $playerType: String!) { "
    "streamPlaybackAccessToken(channelName: $login, params: {platform: \"web\", "
    "playerBackend: \"mediaplayer\", playerType: $playerType}) { value signature } }";

AccessToken fetchStreamToken(GqlClient& gql, const std::string& login,
                             const std::string& playerType) {
  std::string normalized = normalizeLogin(login);
  nlohmann::json data = gql.query(
      kPlaybackTokenQuery, nlohmann::json{{"login", normalized}, {"playerType", playerType}});
  if (!data.is_object()) throw ApiError("streamPlaybackAccessToken missing");
  auto it = data.find("streamPlaybackAccessToken");
  if (it == data.end()) throw ApiError("streamPlaybackAccessToken missing");
  if (it->is_null()) throw NotFoundError("channel not found: " + normalized);
  const auto& t = *it;
  if (!t.is_object() || !t.contains("value") || !t["value"].is_string() ||
      !t.contains("signature") || !t["signature"].is_string())
    throw ApiError("streamPlaybackAccessToken has an unexpected shape");
  return AccessToken{t["value"].get<std::string>(), t["signature"].get<std::string>()};
}

std::string usherStreamUrl(const std::string& login, const AccessToken& token, unsigned nonce) {
  return std::string(kUsherStreamBase) + percentEncode(normalizeLogin(login)) + ".m3u8?" +
         buildQuery({
             {"allow_source", "true"},
             {"p", std::to_string(nonce)},
             {"player_backend", "mediaplayer"},
             {"playlist_include_framerate", "true"},
             {"sig", token.signature},
             {"supported_codecs", "avc1"},
             {"token", token.value},
         });
}

std::vector<Variant> fetchVariants(HttpClient& http, const std::string& masterUrl) {
  HttpRequest req;
  req.url = masterUrl;
  HttpResponse res = http.send(req);
  if (res.status == 404) throw OfflineError("channel is offline");
  if (res.status != 200)
    throw HttpStatusError("usher HTTP " + std::to_string(res.status), res.status);
  return parseMaster(res.body);
}

Variant pickVariant(const std::vector<Variant>& variants, const std::string& preferred) {
  if (variants.empty()) throw ApiError("no variants");

  if (preferred == "source") {
    for (const auto& v : variants)
      if (v.source) return v;
  } else if (preferred != "best" && preferred != "worst") {
    // Three separate passes over all variants: exact name first, then exact label, then
    // label-without-" (source)". A later variant's exact-name match still beats an earlier
    // variant's weaker label match.
    for (const auto& v : variants)
      if (v.name == preferred) return v;
    for (const auto& v : variants)
      if (v.label == preferred) return v;
    for (const auto& v : variants)
      if (stripSourceSuffix(v.label) == preferred) return v;
  }

  // "best"/"worst", the "source" fallback and the unknown-name fallback all land here:
  // prefer video variants over audio-only ones (height == 0), unless every variant is
  // audio-only.
  std::vector<const Variant*> pool;
  for (const auto& v : variants)
    if (v.height > 0) pool.push_back(&v);
  if (pool.empty())
    for (const auto& v : variants) pool.push_back(&v);

  auto byBandwidth = [](const Variant* a, const Variant* b) { return a->bandwidth < b->bandwidth; };
  if (preferred == "worst") return **std::min_element(pool.begin(), pool.end(), byBandwidth);
  return **std::max_element(pool.begin(), pool.end(), byBandwidth);
}

namespace {

// The top of the picker's ordering (qualityMenu()): the best video variant, or
// the best of what there is when every variant is audio only.
const Variant& topQuality(const std::vector<Variant>& variants) {
  const Variant* top = nullptr;
  for (const auto& v : variants)
    if (v.height > 0 && (!top || ranksAbove(v, *top))) top = &v;
  if (top) return *top;
  for (const auto& v : variants)
    if (!top || ranksAbove(v, *top)) top = &v;
  return *top;
}

}  // namespace

Variant pickQuality(const std::vector<Variant>& variants, const std::string& wanted) {
  if (variants.empty()) throw ApiError("no variants");
  if (wanted.empty() || wanted == "auto" || wanted == "best") return topQuality(variants);
  if (wanted == "source") {
    for (const auto& v : variants)
      if (v.source) return v;
    return topQuality(variants);
  }

  for (const auto& v : variants)
    if (v.name == wanted) return v;
  for (const auto& v : variants)
    if (v.label == wanted) return v;
  for (const auto& v : variants)
    if (stripSourceSuffix(v.label) == wanted) return v;

  // Not offered: the closest video variant, the best one at or below it, or
  // else the lowest one above it.
  int height = 0;
  int rate = 0;
  if (parseQualityName(wanted, &height, &rate)) {
    const Variant* below = nullptr;
    const Variant* above = nullptr;
    for (const auto& v : variants) {
      if (v.height <= 0) continue;
      const int r = rateClass(v.fps);
      if (v.height > height || (v.height == height && r > rate)) {
        if (!above || ranksAbove(*above, v)) above = &v;
      } else if (!below || ranksAbove(v, *below)) {
        below = &v;
      }
    }
    if (below) return *below;
    if (above) return *above;
  }
  return topQuality(variants);
}

std::string qualityLabel(const Variant& variant) {
  if (isAudioOnly(variant)) return "Audio only";
  std::string label = variant.label;
  if (label.empty() || label == "chunked") {
    if (variant.height > 0)
      label = std::to_string(variant.height) + "p" + (rateClass(variant.fps) == 60 ? "60" : "");
    else
      label = variant.source ? "Source" : variant.name;
  }
  if (variant.source && label != "Source" && label.find("(source)") == std::string::npos)
    label += " (source)";
  return label;
}

std::vector<QualityOption> qualityMenu(const std::vector<Variant>& variants) {
  std::vector<const Variant*> order;
  order.reserve(variants.size());
  for (const auto& v : variants) order.push_back(&v);
  std::stable_sort(order.begin(), order.end(),
                   [](const Variant* a, const Variant* b) { return ranksAbove(*a, *b); });

  std::vector<QualityOption> menu;
  menu.reserve(order.size());
  for (const Variant* v : order) {
    bool listed = false;
    for (const auto& row : menu) listed = listed || row.name == v->name;
    if (listed) continue;
    QualityOption row;
    row.name = v->name;
    row.label = qualityLabel(*v);
    row.width = v->width;
    row.height = v->height;
    row.fps = static_cast<int>(std::lround(v->fps));
    menu.push_back(std::move(row));
  }
  return menu;
}

}  // namespace twitch
