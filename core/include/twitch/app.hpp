#pragma once

namespace twitch {

// This app's Client ID, from the owner's Public application on
// dev.twitch.tv. A Public client has no secret: this value is not sensitive
// and is safe to embed (it only identifies the app to Twitch, the same way
// kWebClientId in gql.hpp identifies Twitch's own web client).
inline constexpr const char* kAppClientId = "nzmc1l0jlya3s9k53gciqu3dwn1ytw";

// Scopes requested at login: see which channels the user follows, send chat
// messages as them, and make clips of the stream they watch. Reading chat
// stays anonymous (ChatClient), so no chat-read scope is requested.
inline constexpr const char* kAppScopes = "user:read:follows user:write:chat clips:edit";

// The scope HelixClient::createClip needs. Logins from before it was in
// kAppScopes don't have it: the app keeps them, and asks for a new login only
// when the user tries to make a clip (hasScope in auth.hpp).
inline constexpr const char* kClipScope = "clips:edit";

}  // namespace twitch
