#include "twitch/badges.hpp"

#include "twitch/gql.hpp"
#include "twitch/login.hpp"

namespace twitch {

namespace {

const char* kBadgesQuery =
    "query($login:String!){"
    "badges{setID version imageURL(size:DOUBLE)} "
    "user(login:$login){broadcastBadges{setID version imageURL(size:DOUBLE)}}}";

void addAll(BadgeCatalog& c, const nlohmann::json& list) {
  if (!list.is_array()) return;
  for (const auto& b : list) {
    if (!b.is_object()) continue;
    const auto set = b.find("setID");
    const auto version = b.find("version");
    const auto url = b.find("imageURL");
    if (set == b.end() || version == b.end() || url == b.end()) continue;
    if (!set->is_string() || !version->is_string() || !url->is_string()) continue;
    c.add(set->get<std::string>(), version->get<std::string>(), url->get<std::string>());
  }
}

}  // namespace

std::string BadgeCatalog::url(const Badge& badge) const {
  const auto it = urls_.find(badge.set + "/" + badge.version);
  return it == urls_.end() ? std::string() : it->second;
}

void BadgeCatalog::add(const std::string& set, const std::string& version, const std::string& url) {
  urls_[set + "/" + version] = url;
}

BadgeCatalog fetchBadges(GqlClient& gql, const std::string& login) {
  const nlohmann::json data = gql.query(kBadgesQuery, {{"login", normalizeLogin(login)}});
  BadgeCatalog c;
  if (const auto global = data.find("badges"); global != data.end()) addAll(c, *global);
  if (const auto user = data.find("user"); user != data.end() && user->is_object())
    if (const auto own = user->find("broadcastBadges"); own != user->end()) addAll(c, *own);
  return c;
}

}  // namespace twitch
