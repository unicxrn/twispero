#pragma once
#include <string>

#include <nlohmann/json.hpp>

#include "twitch/http.hpp"

namespace twitch {

// Twitch's own web client ID. Works anonymously for public data.
inline constexpr const char* kWebClientId = "kimne78kx3ncx6brgo4mv6wki5h1ko";
inline constexpr const char* kGqlUrl = "https://gql.twitch.tv/gql";

class GqlClient {
 public:
  explicit GqlClient(HttpClient& http, std::string clientId = kWebClientId);

  // Runs one GraphQL query and returns its "data" object.
  // Throws ApiError on non-200, invalid JSON, a non-empty "errors" array, or missing "data".
  nlohmann::json query(const std::string& query,
                       const nlohmann::json& variables = nlohmann::json::object());

 private:
  HttpClient& http_;
  std::string clientId_;
};

}  // namespace twitch
