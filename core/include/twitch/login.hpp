#pragma once
#include <string>

namespace twitch {

// Lowercases ASCII letters, then requires the result to be 1-25 characters of
// [a-z0-9_] (Twitch's own username rules). Throws NotFoundError otherwise.
std::string normalizeLogin(const std::string& login);

}  // namespace twitch
