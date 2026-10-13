#pragma once
#include <map>
#include <string>

#include "twitch/irc.hpp"

namespace twitch {

class GqlClient;  // twitch/gql.hpp; declared here so this header stays free of the JSON library

// Badge pictures by set and version.
class BadgeCatalog {
 public:
  // The picture for a badge, "" when there is none.
  std::string url(const Badge& badge) const;
  // A later add for the same set and version replaces the earlier one.
  void add(const std::string& set, const std::string& version, const std::string& url);
  bool empty() const { return urls_.empty(); }

 private:
  std::map<std::string, std::string> urls_;  // "set/version" -> url
};

// One GQL request: Twitch's global badges with the channel's own (subscriber
// and bits tiers) on top, 36 px pictures. Throws like GqlClient::query, and
// NotFoundError for a login that is not a valid name. A channel that does not
// exist just has no badges of its own.
BadgeCatalog fetchBadges(GqlClient& gql, const std::string& login);

}  // namespace twitch
