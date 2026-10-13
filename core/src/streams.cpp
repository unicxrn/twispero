#include "twitch/streams.hpp"

#include <algorithm>

#include "twitch/error.hpp"
#include "json_helpers.hpp"

namespace twitch {

const char* const kTopStreamsQuery =
    "query TopStreams($first: Int!) { streams(first: $first) { edges { node { "
    "id title viewersCount previewImageURL(width: 440, height: 248) "
    "broadcaster { login displayName } game { name } } } } }";

std::vector<StreamInfo> parseTopStreams(const nlohmann::json& data) {
  auto streams = data.find("streams");
  if (streams == data.end() || !streams->is_object()) throw ApiError("streams missing");
  auto edges = streams->find("edges");
  if (edges == streams->end() || !edges->is_array()) throw ApiError("streams.edges missing");

  std::vector<StreamInfo> out;
  for (const auto& edge : *edges) {
    const nlohmann::json* node = objectOr(&edge, "node");
    if (!node) continue;
    const nlohmann::json* broadcaster = objectOr(node, "broadcaster");
    if (!broadcaster) continue;

    StreamInfo s;
    s.id = stringOr(node, "id");
    s.title = stringOr(node, "title");
    s.viewers = intOr(node, "viewersCount");
    s.previewUrl = stringOr(node, "previewImageURL");
    s.login = stringOr(broadcaster, "login");
    s.displayName = stringOr(broadcaster, "displayName");
    s.gameName = stringOr(objectOr(node, "game"), "name");
    out.push_back(std::move(s));
  }
  return out;
}

// Twitch quirk (seen live, 2026-10): streams(first: n) returns n-1 edges, so
// first: 1 gives none at all. Ask for five more than wanted, capped at 30 (the
// most Twitch accepts), and trim the answer back to `first`.
std::vector<StreamInfo> topStreams(GqlClient& gql, int first) {
  const int want = first > 0 ? first : 0;
  const int ask = std::min(want, 25) + 5;  // never want + 5: that overflows near INT_MAX
  std::vector<StreamInfo> streams =
      parseTopStreams(gql.query(kTopStreamsQuery, nlohmann::json{{"first", ask}}));
  if (streams.size() > static_cast<size_t>(want)) streams.resize(static_cast<size_t>(want));
  return streams;
}

}  // namespace twitch
