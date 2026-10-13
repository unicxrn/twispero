#include "twitch/gql.hpp"

#include "twitch/error.hpp"

namespace twitch {

GqlClient::GqlClient(HttpClient& http, std::string clientId)
    : http_(http), clientId_(std::move(clientId)) {}

nlohmann::json GqlClient::query(const std::string& query, const nlohmann::json& variables) {
  HttpRequest req;
  req.method = "POST";
  req.url = kGqlUrl;
  req.headers = {{"Client-Id", clientId_}, {"Content-Type", "application/json"}};
  // error_handler_t::replace: invalid UTF-8 in any query/variables can never throw here.
  req.body = nlohmann::json{{"query", query}, {"variables", variables}}.dump(
      -1, ' ', false, nlohmann::json::error_handler_t::replace);

  HttpResponse res = http_.send(req);
  if (res.status != 200) throw HttpStatusError("GQL HTTP " + std::to_string(res.status), res.status);

  nlohmann::json body = nlohmann::json::parse(res.body, nullptr, /*allow_exceptions=*/false);
  if (body.is_discarded()) throw ApiError("GQL response is not JSON");

  if (!body.is_object()) throw ApiError("GQL response has no data");

  auto errors = body.find("errors");
  if (errors != body.end() && errors->is_array() && !errors->empty()) {
    const auto& first = (*errors)[0];
    std::string message = "unknown error";
    if (first.is_object() && first.contains("message") && first["message"].is_string())
      message = first["message"].get<std::string>();
    throw ApiError("GQL error: " + message);
  }

  auto data = body.find("data");
  if (data == body.end() || data->is_null()) throw ApiError("GQL response has no data");
  return *data;
}

}  // namespace twitch
