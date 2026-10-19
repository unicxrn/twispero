#pragma once
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "twitch/gql.hpp"

namespace twitch {

struct StreamInfo {
  std::string id;
  std::string title;
  std::string login;        // lowercase channel name, used in URLs and chat
  std::string displayName;  // what the UI shows
  std::string gameName;     // empty when the stream has no category
  std::string previewUrl;   // 440x248 JPEG
  int viewers = 0;
};

extern const char* const kTopStreamsQuery;

// Parses the "data" object of kTopStreamsQuery. Skips nodes without a broadcaster.
// Throws ApiError when data.streams or data.streams.edges is missing.
std::vector<StreamInfo> parseTopStreams(const nlohmann::json& data);

// Live streams in the order Twitch returns them (most viewers first). Returns at
// most `first` streams; asks Twitch for a few extra because it currently returns
// one fewer than requested. Twitch caps a request at 30. `language` is a
// broadcaster language from browse.hpp ("" for every language); anything else
// is ignored and the list is unfiltered.
std::vector<StreamInfo> topStreams(GqlClient& gql, int first, const std::string& language = {});

}  // namespace twitch
