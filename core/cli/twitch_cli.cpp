#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "twitch/app.hpp"
#include "twitch/auth.hpp"
#include "twitch/browse.hpp"
#include "twitch/chat.hpp"
#include "twitch/curl/curl_http.hpp"
#include "twitch/curl/curl_websocket.hpp"
#include "twitch/error.hpp"
#include "twitch/gql.hpp"
#include "twitch/helix.hpp"
#include "twitch/hls.hpp"
#include "twitch/login.hpp"
#include "twitch/playback.hpp"
#include "twitch/streams.hpp"
#include "twitch/videos.hpp"

namespace {

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

constexpr const char* kEllipsis = "\xE2\x80\xA6";
constexpr int kTopCandidates = 5;

int usage() {
  std::fprintf(stderr,
               "usage:\n"
               "  twitch-cli top [count]\n"
               "  twitch-cli play <channel> [quality|best|worst|source] [playerType]\n"
               "  twitch-cli chat <channel>\n"
               "  twitch-cli resolve [channel]\n"
               "  twitch-cli vod <id>\n"
               "  twitch-cli clip <slug>\n"
               "  twitch-cli categories [count]\n"
               "  twitch-cli category <name> [count]\n"
               "  twitch-cli videos <login> [archive|highlight|upload]\n"
               "  twitch-cli clips <login> [day|week|month|all]\n"
               "  twitch-cli search <query...>\n"
               "  twitch-cli channel <login>\n"
               "  twitch-cli login\n"
               "  twitch-cli me\n"
               "  twitch-cli following\n"
               "  twitch-cli say <channel> <text...>\n"
               "  twitch-cli logout\n");
  return 2;
}

// ---- auth.json: ~/.config/ps5-twitch/auth.json (XDG_CONFIG_HOME respected) ----
//
// Saved as the serializeLogin() JSON, directory mode 0700, file mode 0600,
// written atomically (temp file in the same directory, then rename). Never
// printed: callers only ever show a login/display name, not a token.

std::string xdgConfigHome() {
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && *xdg != '\0') return xdg;
  const char* home = std::getenv("HOME");
  return std::string(home != nullptr ? home : ".") + "/.config";
}

std::string authDir() { return xdgConfigHome() + "/ps5-twitch"; }
std::string authPath() { return authDir() + "/auth.json"; }

// Creates authDir() (and, best effort, its parent) with mode 0700. Fine if
// either already exists; chmod runs regardless so an existing directory
// with looser permissions still ends up at 0700.
void ensureAuthDir() {
  ::mkdir(xdgConfigHome().c_str(), 0755);  // parent; ignore the result, may pre-exist
  const std::string dir = authDir();
  if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST)
    throw std::runtime_error("could not create " + dir + ": " + std::strerror(errno));
  ::chmod(dir.c_str(), 0700);
}

std::optional<std::string> readFile(const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) return std::nullopt;
  std::string data;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
  std::fclose(f);
  return data;
}

// Writes `data` to `path` atomically: a temp file in the same directory
// (so the rename can't cross filesystems), chmod'd to `mode` to defeat the
// umask, then renamed over `path`.
void writeFileAtomic(const std::string& path, const std::string& data, mode_t mode) {
  const std::string tmp = path + ".tmp." + std::to_string(static_cast<long>(::getpid()));
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode);
  if (fd < 0) throw std::runtime_error("could not open " + tmp + ": " + std::strerror(errno));

  size_t written = 0;
  while (written < data.size()) {
    const ssize_t n = ::write(fd, data.data() + written, data.size() - written);
    if (n < 0) {
      ::close(fd);
      ::unlink(tmp.c_str());
      throw std::runtime_error(std::string("write failed: ") + std::strerror(errno));
    }
    written += static_cast<size_t>(n);
  }
  ::close(fd);
  ::chmod(tmp.c_str(), mode);

  if (::rename(tmp.c_str(), path.c_str()) != 0) {
    ::unlink(tmp.c_str());
    throw std::runtime_error(std::string("rename failed: ") + std::strerror(errno));
  }
}

std::optional<twitch::SavedLogin> readLoginFile() {
  std::optional<std::string> data = readFile(authPath());
  if (!data) return std::nullopt;
  return twitch::parseLogin(*data);
}

void writeLoginFile(const twitch::SavedLogin& login) {
  ensureAuthDir();
  writeFileAtomic(authPath(), twitch::serializeLogin(login), 0600);
}

void removeLoginFile() { ::unlink(authPath().c_str()); }

long long nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string joinSpace(const std::vector<std::string>& items) {
  std::string out;
  for (size_t i = 0; i < items.size(); ++i) {
    if (i != 0) out += ' ';
    out += items[i];
  }
  return out;
}

constexpr long long kRefreshWindowMs = 5 * 60 * 1000;  // refresh within 5 minutes of expiry

// Loads the saved login, refreshing it first (and saving the rotated tokens
// right away - a refresh token works only once) when the access token
// expires within kRefreshWindowMs. Used by me/following/say so none of them
// has to duplicate the refresh dance. Throws UnauthorizedError, with the
// same message, whether there's no saved login at all or the saved refresh
// token is dead (in which case the stale file is removed too) - callers
// don't need to tell those apart, both mean "run twitch-cli login".
twitch::SavedLogin loadAndRefresh(twitch::HttpClient& http) {
  std::optional<twitch::SavedLogin> saved = readLoginFile();
  if (!saved) throw twitch::UnauthorizedError("not logged in - run 'twitch-cli login'");

  const long long now = nowMs();
  if (saved->tokens.expiresAtMs - now < kRefreshWindowMs) {
    twitch::AuthClient auth(http, twitch::kAppClientId);
    try {
      saved->tokens = auth.refresh(saved->tokens.refresh, now);
      writeLoginFile(*saved);
    } catch (const twitch::UnauthorizedError&) {
      removeLoginFile();
      throw twitch::UnauthorizedError("not logged in - run 'twitch-cli login'");
    }
  }
  return *saved;
}

int cmdTop(int count) {
  twitch::CurlHttpClient http;
  twitch::GqlClient gql(http);
  for (const auto& s : twitch::topStreams(gql, count)) {
    std::printf("%8d  %-20s %-24s %s\n", s.viewers, s.login.c_str(),
                s.gameName.empty() ? "-" : s.gameName.c_str(), s.title.c_str());
  }
  return 0;
}

int cmdCategories(int count) {
  twitch::CurlHttpClient http;
  twitch::GqlClient gql(http);
  for (const auto& c : twitch::topCategories(gql, count)) {
    std::printf("%8d  %-30s %s\n", c.viewers, c.name.c_str(), c.boxArtUrl.c_str());
  }
  return 0;
}

int cmdCategory(const std::string& name, int count) {
  twitch::CurlHttpClient http;
  twitch::GqlClient gql(http);
  for (const auto& s : twitch::categoryStreams(gql, name, count)) {
    std::printf("%8d  %-20s %s\n", s.viewers, s.login.c_str(), s.title.c_str());
  }
  return 0;
}

int cmdSearch(const std::string& query) {
  twitch::CurlHttpClient http;
  twitch::GqlClient gql(http);

  std::printf("channels:\n");
  for (const auto& h : twitch::searchChannels(gql, query, 10)) {
    std::printf("  %8d  %-20s %s\n", h.liveViewers, h.login.c_str(), h.displayName.c_str());
  }
  std::printf("categories:\n");
  for (const auto& c : twitch::searchCategories(gql, query, 10)) {
    std::printf("  %8d  %-30s %s\n", c.viewers, c.name.c_str(), c.boxArtUrl.c_str());
  }
  return 0;
}

int cmdChannel(const std::string& login) {
  twitch::CurlHttpClient http;
  twitch::GqlClient gql(http);
  twitch::ChannelInfo c = twitch::fetchChannel(gql, login);

  std::printf("channel:    %s (%s)\n", c.displayName.c_str(), c.login.c_str());
  std::printf("followers:  %d\n", c.followers);
  std::printf("description: %s\n", c.description.empty() ? "-" : c.description.c_str());
  std::printf("avatar:     %s\n", c.profileImageUrl.c_str());
  const long long now = static_cast<long long>(std::time(nullptr));
  if (c.live) {
    std::printf("live:       yes, %d viewers, playing %s\n", c.viewers,
                c.gameName.empty() ? "-" : c.gameName.c_str());
    std::printf("title:      %s\n", c.streamTitle.c_str());
    std::printf("started:    %s (%s)\n", c.streamStartedAt.empty() ? "-" : c.streamStartedAt.c_str(),
                twitch::uptimeText(c.streamStartedAt, now).c_str());
    const std::string lang = twitch::languageName(c.language);
    std::printf("language:   %s%s%s\n", c.language.empty() ? "-" : c.language.c_str(), lang.empty() ? "" : " = ",
                lang.c_str());
    std::string tags;
    for (const std::string& t : c.tags) tags += (tags.empty() ? "" : ", ") + t;
    std::printf("tags:       %s\n", tags.empty() ? "-" : tags.c_str());
  } else {
    std::printf("live:       no\n");
  }
  const std::vector<twitch::ScheduleSegment> next = twitch::fetchSchedule(gql, login, now, 3);
  if (next.empty()) std::printf("schedule:   -\n");
  for (const twitch::ScheduleSegment& s : next)
    std::printf("schedule:   %s  %-14s %s%s%s\n", s.startAt.c_str(), twitch::startsInText(s.startAt, now).c_str(),
                s.title.empty() ? "-" : s.title.c_str(), s.category.empty() ? "" : " / ", s.category.c_str());
  return 0;
}

int cmdPlay(const std::string& channel, const std::string& quality, const std::string& playerType) {
  twitch::CurlHttpClient http;
  twitch::GqlClient gql(http);

  auto token = twitch::fetchStreamToken(gql, channel, playerType);
  std::random_device rd;
  std::string master = twitch::usherStreamUrl(channel, token, rd() % 1000000);
  auto variants = twitch::fetchVariants(http, master);

  std::printf("qualities:");
  for (const auto& v : variants) {
    std::printf(" %s", v.label.c_str());
    if (v.name != v.label) std::printf(" [%s]", v.name.c_str());
  }
  std::printf("\n");

  twitch::Variant chosen = twitch::pickVariant(variants, quality);
  std::printf("playing %s (%dx%d, %.0f fps, %d bps)\n", chosen.label.c_str(), chosen.width,
              chosen.height, chosen.fps, chosen.bandwidth);

  twitch::HttpRequest req;
  req.url = chosen.uri;
  auto res = http.send(req);
  if (res.status != 200) throw twitch::ApiError("media playlist HTTP " + std::to_string(res.status));
  auto media = twitch::parseMedia(res.body);

  std::printf("target duration %.1f s, %zu segments, ads: %s\n", media.targetDuration,
              media.segments.size(), media.containsAds() ? "YES" : "no");
  for (const auto& s : media.segments) {
    std::printf("  #%lld %.3fs %s%s\n", s.sequence, s.duration, s.ad ? "[AD] " : "",
                s.title.c_str());
  }
  return 0;
}

// Host-side mirror of the PS5 launcher's resolve worker (resolve() in
// app/projects/evoplayer/twitch/src/evo_twitch_launcher.cpp): same sequence,
// normalizeLogin -> fetchStreamToken(gql, login, "embed") ->
// usherStreamUrl -> fetchVariants -> pickVariant(variants, "best"). With no
// channel it walks topStreams(gql, 5) and tries candidates in order,
// skipping the same three errors the launcher skips and stopping on
// anything else (including NetworkError). The one addition over the
// launcher is the ad check: it fetches the chosen variant's media playlist
// once, with the same http client, purely to report ads. Never prints the
// master or media playlist URL: both carry signed tokens.
int cmdResolve(const std::string& channelArg) {
  twitch::CurlHttpClient http;
  twitch::GqlClient gql(http);
  std::random_device rd;

  auto resolveAndPrint = [&](const std::string& login, const std::string& displayName,
                             const std::string& title) {
    const twitch::AccessToken token = twitch::fetchStreamToken(gql, login, "embed");
    const std::string master = twitch::usherStreamUrl(login, token, rd() % 1000000);
    const std::vector<twitch::Variant> variants = twitch::fetchVariants(http, master);
    const twitch::Variant chosen = twitch::pickVariant(variants, "best");

    twitch::HttpRequest req;
    req.url = chosen.uri;
    const twitch::HttpResponse res = http.send(req);
    if (res.status != 200)
      throw twitch::HttpStatusError("media playlist HTTP " + std::to_string(res.status),
                                     res.status);
    const bool ads = twitch::parseMedia(res.body).containsAds();

    const std::string& name = displayName.empty() ? login : displayName;
    std::printf("channel:  %s (%s)\n", name.c_str(), login.c_str());
    std::printf("title:    %s\n", title.empty() ? "-" : title.c_str());
    std::printf("quality:  %s", chosen.label.c_str());
    if (chosen.name != chosen.label) std::printf(" [%s]", chosen.name.c_str());
    std::printf(" %dx%d %.0f fps\n", chosen.width, chosen.height, chosen.fps);
    std::printf("ads:      %s\n", ads ? "YES" : "no");
  };

  // A channel on the command line: that one or nothing, same as the
  // launcher's channel-file path.
  if (!channelArg.empty()) {
    const std::string login = twitch::normalizeLogin(channelArg);
    resolveAndPrint(login, "", "");
    return 0;
  }

  // Otherwise the most-watched stream that actually resolves.
  const std::vector<twitch::StreamInfo> top = twitch::topStreams(gql, kTopCandidates);
  if (top.empty()) throw twitch::NotFoundError("no live streams");

  std::string lastError;
  for (const twitch::StreamInfo& s : top) {
    std::fprintf(stderr, "trying %s %s", s.login.c_str(), kEllipsis);
    std::fflush(stderr);
    try {
      const std::string login = twitch::normalizeLogin(s.login);
      resolveAndPrint(login, s.displayName, s.title);
      std::fprintf(stderr, "\n");
      return 0;
    } catch (const twitch::NotFoundError& e) {
      lastError = e.what();
    } catch (const twitch::OfflineError& e) {
      lastError = e.what();
    } catch (const twitch::HttpStatusError& e) {
      lastError = e.what();
    }
    std::fprintf(stderr, " skipped: %s\n", lastError.c_str());
  }
  throw twitch::NotFoundError("none of the top streams would open (" + lastError + ")");
}

twitch::VideoType parseVideoType(const std::string& s) {
  if (s.empty() || s == "archive") return twitch::VideoType::Archive;
  if (s == "highlight") return twitch::VideoType::Highlight;
  if (s == "upload") return twitch::VideoType::Upload;
  throw twitch::ApiError("unknown video type: " + s);
}

twitch::ClipPeriod parseClipPeriod(const std::string& s) {
  if (s.empty() || s == "day") return twitch::ClipPeriod::Day;
  if (s == "week") return twitch::ClipPeriod::Week;
  if (s == "month") return twitch::ClipPeriod::Month;
  if (s == "all") return twitch::ClipPeriod::All;
  throw twitch::ApiError("unknown clip period: " + s);
}

int cmdVideos(const std::string& login, const std::string& typeArg) {
  twitch::CurlHttpClient http;
  twitch::GqlClient gql(http);
  const twitch::VideoType type = parseVideoType(typeArg);
  const twitch::VideoPage page = twitch::fetchChannelVideos(gql, login, type, 20);
  const long long now = nowMs() / 1000;

  for (const auto& v : page.cards) {
    std::printf("%-12s %6d views  %5ds  %-14s %-20s %s\n", v.id.c_str(), v.viewCount,
                v.lengthSeconds, twitch::ageText(v.publishedAt, now).c_str(),
                v.gameName.empty() ? "-" : v.gameName.c_str(), v.title.c_str());
  }
  std::printf("next: %s\n", page.nextCursor.empty() ? "-" : page.nextCursor.c_str());
  return 0;
}

int cmdClips(const std::string& login, const std::string& periodArg) {
  twitch::CurlHttpClient http;
  twitch::GqlClient gql(http);
  const twitch::ClipPeriod period = parseClipPeriod(periodArg);
  const twitch::ClipPage page = twitch::fetchChannelClips(gql, login, period, 20);
  const long long now = nowMs() / 1000;

  for (const auto& c : page.cards) {
    std::printf("%-26s %6d views  %4ds  %-14s %-16s %s\n", c.slug.c_str(), c.viewCount,
                c.durationSeconds, twitch::ageText(c.createdAt, now).c_str(),
                c.curatorName.c_str(), c.title.c_str());
  }
  std::printf("next: %s\n", page.nextCursor.empty() ? "-" : page.nextCursor.c_str());
  return 0;
}

int cmdVod(const std::string& videoId) {
  twitch::CurlHttpClient http;
  twitch::GqlClient gql(http);
  const std::vector<twitch::Variant> variants = twitch::resolveVod(gql, http, videoId);

  std::printf("qualities:");
  for (const auto& v : variants) {
    std::printf(" %s", v.label.c_str());
    if (v.name != v.label) std::printf(" [%s]", v.name.c_str());
  }
  std::printf("\n");

  const twitch::Variant chosen = twitch::pickQuality(variants, "best");
  std::printf("best: %s (%dx%d, %.0f fps)\n", chosen.label.c_str(), chosen.width, chosen.height,
              chosen.fps);
  return 0;
}

int cmdClip(const std::string& slug) {
  twitch::CurlHttpClient http;
  twitch::GqlClient gql(http);
  const std::vector<twitch::ClipVariant> variants = twitch::clipVariants(gql, slug);
  for (const auto& v : variants) {
    std::printf("%-6s %3dfps %s\n", v.quality.c_str(), v.fps, v.url.c_str());
  }
  return 0;
}

int cmdChat(const std::string& channel) {
  twitch::CurlWebSocket ws;
  twitch::ChatClient chat(ws);
  std::random_device rd;
  chat.connectAnonymous(10000 + rd() % 90000);
  chat.join(channel);
  std::fprintf(stderr, "joined #%s, Ctrl+C to quit\n", channel.c_str());
  while (true) {
    for (const auto& m : chat.poll(1000)) {
      if (m.action) {
        std::printf("* %s %s\n", m.displayName.c_str(), m.text.c_str());
      } else {
        std::printf("%s: %s\n", m.displayName.c_str(), m.text.c_str());
      }
      std::fflush(stdout);
    }
  }
}

// Device-code login: prints the code and URL, then polls until the owner
// approves (or denies, or lets it expire) on their phone. Live run only -
// the coordinator runs this one with the owner at the keyboard.
int cmdLogin() {
  twitch::CurlHttpClient http;
  twitch::AuthClient auth(http, twitch::kAppClientId);

  const twitch::DeviceCode code = auth.startDeviceLogin(twitch::kAppScopes);
  // Plain, no hyphen: typing "ABCD-EFGH" by hand on twitch.tv/activate may
  // not be accepted.
  std::printf("Open %s and approve (code %s)\n", code.verificationUri.c_str(),
              code.userCode.c_str());
  std::fflush(stdout);

  int intervalSec = code.intervalSec > 0 ? code.intervalSec : 5;
  const long long deadlineMs = nowMs() + static_cast<long long>(code.expiresInSec) * 1000;

  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(intervalSec));
    const long long now = nowMs();
    if (now >= deadlineMs) throw twitch::ApiError("login code expired");

    twitch::Tokens tokens;
    const twitch::PollResult result = auth.pollDeviceLogin(code, twitch::kAppScopes, now, &tokens);
    if (result == twitch::PollResult::Pending) continue;
    if (result == twitch::PollResult::SlowDown) {
      intervalSec += 5;
      continue;
    }
    if (result == twitch::PollResult::Expired) throw twitch::ApiError("login code expired");
    if (result == twitch::PollResult::Denied) throw twitch::ApiError("login denied");

    // Done: validate (Twitch requires this right after getting a token) and
    // ask Helix for the display name validate doesn't carry.
    const twitch::Identity identity = auth.validate(tokens.access);
    twitch::HelixClient helix(http, twitch::kAppClientId, tokens.access);
    const twitch::HelixUser me = helix.me();

    twitch::SavedLogin saved;
    saved.tokens = tokens;
    saved.userId = identity.userId.empty() ? me.id : identity.userId;
    saved.login = identity.login.empty() ? me.login : identity.login;
    saved.displayName = me.displayName;
    writeLoginFile(saved);

    const std::string& shown = saved.displayName.empty() ? saved.login : saved.displayName;
    std::printf("Logged in as %s (%s)\n", shown.c_str(), saved.login.c_str());
    return 0;
  }
}

int cmdMe() {
  twitch::CurlHttpClient http;
  const twitch::SavedLogin saved = loadAndRefresh(http);

  twitch::AuthClient auth(http, twitch::kAppClientId);
  const twitch::Identity identity = auth.validate(saved.tokens.access);

  std::printf("login:   %s\n", identity.login.c_str());
  std::printf("user id: %s\n", identity.userId.c_str());
  std::printf("scopes:  %s\n", joinSpace(identity.scopes).c_str());
  std::printf("expires: %d min\n", identity.expiresInSec / 60);
  return 0;
}

int cmdFollowing() {
  twitch::CurlHttpClient http;
  const twitch::SavedLogin saved = loadAndRefresh(http);
  twitch::HelixClient helix(http, twitch::kAppClientId, saved.tokens.access);

  const long long nowSec = nowMs() / 1000;
  const std::vector<twitch::FollowedStream> live = helix.followedStreams(saved.userId, nowSec);
  const std::vector<twitch::FollowedChannel> all = helix.followedChannels(saved.userId);

  std::unordered_set<std::string> liveIds;
  for (const auto& s : live) liveIds.insert(s.userId);

  std::printf("live now (%zu):\n", live.size());
  for (const auto& s : live) {
    const std::string& name = s.displayName.empty() ? s.login : s.displayName;
    std::printf("  %-20s %6d viewers  %-20s %s\n", name.c_str(), s.viewers,
                s.game.empty() ? "-" : s.game.c_str(), s.title.c_str());
  }

  std::vector<std::string> offlineNames;
  for (const auto& c : all) {
    if (liveIds.count(c.userId)) continue;
    offlineNames.push_back(c.displayName.empty() ? c.login : c.displayName);
  }
  std::printf("offline (%zu):\n", offlineNames.size());
  for (const auto& name : offlineNames) std::printf("  %s\n", name.c_str());
  return 0;
}

int cmdSay(const std::string& channel, const std::string& text) {
  twitch::CurlHttpClient http;
  const twitch::SavedLogin saved = loadAndRefresh(http);
  twitch::HelixClient helix(http, twitch::kAppClientId, saved.tokens.access);

  const std::string broadcasterId = helix.userIdForLogin(channel);
  const twitch::SendResult result = helix.sendChat(broadcasterId, saved.userId, text);
  if (result.sent) {
    std::printf("sent\n");
  } else {
    std::printf("not sent: %s\n", result.dropReason.empty() ? "unknown reason" : result.dropReason.c_str());
  }
  return 0;
}

int cmdLogout() {
  const std::optional<twitch::SavedLogin> saved = readLoginFile();
  if (!saved) {
    std::printf("not logged in\n");
    return 0;
  }

  twitch::CurlHttpClient http;
  twitch::AuthClient auth(http, twitch::kAppClientId);
  auth.revoke(saved->tokens.access);  // best effort, never throws
  removeLoginFile();
  std::printf("logged out\n");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return usage();
  twitch::curlGlobalInit();
  std::string cmd = argv[1];
  try {
    if (cmd == "top") return cmdTop(argc > 2 ? std::atoi(argv[2]) : 10);
    if (cmd == "play" && argc >= 3)
      return cmdPlay(lower(argv[2]), argc > 3 ? argv[3] : "best", argc > 4 ? argv[4] : "site");
    if (cmd == "chat" && argc >= 3) return cmdChat(lower(argv[2]));
    if (cmd == "resolve") return cmdResolve(argc > 2 ? argv[2] : "");
    if (cmd == "vod" && argc >= 3) return cmdVod(argv[2]);
    if (cmd == "clip" && argc >= 3) return cmdClip(argv[2]);
    if (cmd == "categories") return cmdCategories(argc > 2 ? std::atoi(argv[2]) : 10);
    if (cmd == "category" && argc >= 3)
      return cmdCategory(argv[2], argc > 3 ? std::atoi(argv[3]) : 10);
    if (cmd == "videos" && argc >= 3) return cmdVideos(lower(argv[2]), argc > 3 ? argv[3] : "");
    if (cmd == "clips" && argc >= 3) return cmdClips(lower(argv[2]), argc > 3 ? argv[3] : "");
    if (cmd == "search" && argc >= 3) {
      std::string query = argv[2];
      for (int i = 3; i < argc; ++i) {
        query += ' ';
        query += argv[i];
      }
      return cmdSearch(query);
    }
    if (cmd == "channel" && argc >= 3) return cmdChannel(argv[2]);
    if (cmd == "login") return cmdLogin();
    if (cmd == "me") return cmdMe();
    if (cmd == "following") return cmdFollowing();
    if (cmd == "say" && argc >= 4) {
      std::string text = argv[3];
      for (int i = 4; i < argc; ++i) {
        text += ' ';
        text += argv[i];
      }
      return cmdSay(argv[2], text);
    }
    if (cmd == "logout") return cmdLogout();
    return usage();
  } catch (const twitch::TwitchError& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "unexpected error: %s\n", e.what());
    return 1;
  }
}
