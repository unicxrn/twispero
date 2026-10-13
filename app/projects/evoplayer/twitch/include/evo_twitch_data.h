#pragma once
/*
 * evo_twitch_data.h - what the Twitch screens show: top streams, categories,
 * search results, channel pages, the channel playing, who the logged-in user
 * follows, and a channel's videos and clips or a category's clips.
 *
 * A screen asks with request_*() (on entering, or when the user presses
 * Triangle) and reads the slot every frame. Worker threads run the requests
 * against Twitch's GQL API, or Helix with the user's token for the follows;
 * data_frame() moves finished results into the slots on the main
 * thread, so a slot never changes while a screen is drawing it.
 *
 * Threads: every function here is main thread only. The slots are main-thread
 * state; a worker only ever sees its own copy of a request.
 *
 * Slots. Each accessor returns the same object for the life of the app, so a
 * screen may keep the reference. Its contents change only inside request_*()
 * and data_frame(); don't keep pointers into `value` across those calls.
 *   status  Idle     nothing asked for yet
 *           Loading  asked for, no value yet (draw skeletons)
 *           Ready    `value` is the latest answer
 *           Error    the request failed and there is no value to show;
 *                    `error` says why in a few words
 *   stamp   monotonic milliseconds when `value` landed (0 when it has none),
 *           on the clock data_now_ms() reads. Screens refresh on entry when
 *           data_now_ms() - stamp > 60000.
 *
 * Requests. Two workers run them, each one at a time, oldest first: one for
 * the follows (which can wait on the login service), one for the rest. Per
 * slot only the newest request counts:
 *   - a request queued behind another for the same slot replaces it;
 *   - for the slots that take an argument (category, search, channel, now
 *     playing, the follows, videos and clips), a new argument empties the
 *     slot (status Loading), and an answer that arrives for an older argument
 *     is dropped;
 *   - the same argument again while it is still loading (or already being
 *     refreshed) does nothing;
 *   - the same argument again when it is Ready refreshes in the background:
 *     the old value stays and the status stays Ready until the new one lands;
 *   - the same argument again after an Error retries (status Loading).
 * Meant for events (entering a screen, Triangle), not for every frame.
 *
 * Errors. A failed refresh keeps the old value and status Ready and only sets
 * `error`. A new request for the slot clears it (and the next successful
 * answer), so each failed refresh shows as `error` going from "" to its
 * reason; a screen can toast that while the old value stays up. The full
 * reason goes to the log. `error` is one of: "No connection", "This channel is offline",
 * "Not found", "Twitch didn't answer as expected", "Something went wrong";
 * for the follows also "Log in again" (Twitch refused the token and the
 * refresh, or nobody is logged in), "Log in again from Settings" (the
 * token lacks the follows scope, which only a new login grants), and
 * "Check the PS5's date and time" (a certificate refused for its dates,
 * nearly always the console's clock rather than Twitch).
 *
 * Search fills two slots from one request; each lands with its own status, so
 * the channels can show while the categories failed, or the other way round.
 * An empty or blank query is Ready at once with no results and no request.
 *
 * Thumbnail URLs (StreamCard::thumbUrl) already carry the 5-minute bucket
 * (twitch::liveThumbnailUrl), so the image service fetches a new picture after
 * a refresh that lands in a later bucket. Display names fall back to the login.
 *
 * Paged slots (a channel's videos and clips). A request asks for the first
 * page, as above: a refresh starts the list again from the top.
 * request_more_*() asks for the next page of the list the slot holds, which
 * is appended when it lands; the cursor Twitch hands out for it stays in the
 * service, never in an argument. It asks for nothing unless the slot is
 * Ready with `more` set and nothing for it is on its way, so it is safe to
 * call on every move near the end of a grid. A next page that fails keeps
 * the list (status Ready) and sets `error` (one toast, as for a refresh);
 * asking again retries it. A first page that lands meanwhile (a refresh)
 * drops a next page still on its way. *_loading_more() is true from the ask
 * until the page has landed, failed or been dropped.
 */

#include <string>
#include <vector>

namespace evo_twitch {

struct StreamCard { std::string login, displayName, title, game, thumbUrl; int viewers = 0; };
struct CategoryCard { std::string name, boxArtUrl; int viewers = -1; };
struct ChannelCard { std::string login, displayName, avatarUrl; int liveViewers = -1; };
// One stream on a channel's schedule (stage 7): when it starts (ISO 8601 UTC),
// its title and its category, each "" when the streamer gave none.
struct ScheduleItem { std::string startAt, title, category; };
// thumbUrl: the live preview at 800x450 (the size the page draws it) with the 5-minute bucket, "" when offline.
// Stage 7, while live: startedAt, when the stream started (ISO 8601, "" when
// Twitch didn't say; see uptime_text()); tags, the stream's language by name
// ("English") unless a tag already says it, then the streamer's tags. The
// channel page's slot also carries the next scheduled streams, soonest first,
// up to 3 (empty when there are none or the schedule couldn't be had: the
// page doesn't fail for it); now_playing()'s never does.
struct ChannelPage {
  std::string login, displayName, description, avatarUrl, title, game, thumbUrl;
  int followers = -1, viewers = -1;
  bool live = false;
  std::string startedAt;
  std::vector<std::string> tags;
  std::vector<ScheduleItem> schedule;
};
enum class Status { Idle, Loading, Ready, Error };
template <class T> struct Result { Status status = Status::Idle; T value{}; std::string error; unsigned long long stamp = 0; };

// The 20 most-watched live streams.
const Result<std::vector<StreamCard>>& home_streams() noexcept;        void request_home_streams() noexcept;      // top 20
// The 30 most-watched categories (285x380 box art).
const Result<std::vector<CategoryCard>>& home_categories() noexcept;   void request_home_categories() noexcept;   // top 30
// The 30 most-watched live streams in one category, by its name.
const Result<std::vector<StreamCard>>& category_streams() noexcept;    void request_category_streams(const std::string& name) noexcept; // 30
// Up to 10 categories (144x192 box art) and 10 channels (70x70 avatars) for a query.
const Result<std::vector<CategoryCard>>& search_categories() noexcept;
const Result<std::vector<ChannelCard>>& search_channels() noexcept;    void request_search(const std::string& query) noexcept;          // 10 + 10
// One channel's profile (150x150 avatar), and its stream when it is live. A
// login that does not exist (or is not a valid name) ends in Error "Not found".
const Result<ChannelPage>& channel_page() noexcept;                    void request_channel(const std::string& login) noexcept;
// The same for the channel playing in the player, in a slot of its own so
// the Channel screen's isn't disturbed. The player asks again every minute
// for the viewer count (and a title or category that changed).
const Result<ChannelPage>& now_playing() noexcept;                     void request_now_playing(const std::string& login) noexcept;

// Who the logged-in user follows, through Helix with their token
// (evo_twitch_auth.h). The argument is the user id from auth_view() on the
// main thread, never the token; the worker takes the token itself and checks
// it is still that user's. An empty id asks for nothing and empties the slot.
// A 401 is reported to the login service, which refreshes the token; the
// request is then tried once more. When the user logs out (or another one
// logs in), data_frame() empties both slots and drops answers on their way.
//
// The followed channels that are live now, most viewers first, up to 100.
const Result<std::vector<StreamCard>>& followed_live() noexcept;
void request_followed_live(const std::string& userId) noexcept;
// Every followed channel, up to 300, the newest follow first, with a 150x150
// avatar; the live ones have liveViewers >= 0 (from their own check, which
// can be a little older or newer than followed_live()'s).
const Result<std::vector<ChannelCard>>& followed_channels() noexcept;
void request_followed_channels(const std::string& userId) noexcept;
// True once a follows request ended in "Log in again from Settings" (the
// token lacks the follows scope). Asking again for the same user then asks
// Twitch nothing and the Error stays, until the user logs out (which empties
// the slots and this with them).
bool followed_scope_missing() noexcept;

// ---- Videos and clips (stage 7) ----

// A channel's Videos tab (Twitch's own three kinds) and a clip list's time window.
enum class VideoFilter { Archive, Highlight, Upload };  // Past broadcasts, Highlights, Uploads
enum class ClipPeriod { Day, Week, Month, All };        // Last 24 hours, 7 days, 30 days, All time

// thumbUrl: 440x248, the size the cards draw. age: "3 days ago" as of when
// the page landed, "" when Twitch's date didn't parse; published: the date as
// Twitch sent it (ISO 8601), which a video's saved progress keeps.
struct VideoCard { std::string id, title, game, thumbUrl, age, published; int seconds = 0, views = 0; };
// thumbUrl: 440x248 too (the CDN's copy of Twitch's 1920x1080 one). The
// channel is whose clip it is (category clips come from many); its name
// falls back to the login, and both are "" when Twitch didn't say (a closed
// account).
struct ClipCard {
  std::string slug, title, game, thumbUrl, age, curator, channelLogin, channelName;
  int seconds = 0, views = 0;
};
// A list that comes in pages: `more` while Twitch has another one and the
// list is under its cap.
template <class T> struct Paged { std::vector<T> cards; bool more = false; };

// One channel's past broadcasts, highlights or uploads, newest first: 30 to a
// page, up to 300. A login that doesn't exist ends in Error "Not found".
const Result<Paged<VideoCard>>& channel_videos() noexcept;
void request_channel_videos(const std::string& login, VideoFilter filter) noexcept;
void request_more_channel_videos() noexcept;
bool channel_videos_loading_more() noexcept;
// One channel's clips from a time window, most viewed first: 30 to a page, up to 300.
const Result<Paged<ClipCard>>& channel_clips() noexcept;
void request_channel_clips(const std::string& login, ClipPeriod period) noexcept;
void request_more_channel_clips() noexcept;
bool channel_clips_loading_more() noexcept;
// A category's clips from a time window, most viewed first: one page of 30
// (`more` stays false). A category that doesn't exist ends in Error "Not found".
const Result<Paged<ClipCard>>& category_clips() noexcept;
void request_category_clips(const std::string& name, ClipPeriod period) noexcept;

// Now, in monotonic milliseconds: the clock `stamp` is on. Any thread.
unsigned long long data_now_ms() noexcept;

// "Live for 2 h 14 min" for a stream that started at startedAt (a
// ChannelPage's), as of the console's clock now; "" when it can't say
// (twitch::uptimeText). Any thread.
std::string uptime_text(const std::string& startedAt) noexcept;
// "In 5 h", "In 2 days" for a scheduled stream's start, as of now; "" when it
// can't say (twitch::startsInText). Any thread.
std::string starts_in_text(const std::string& startAt) noexcept;

// Spawns the workers once; later calls do nothing. Call after evo_net is up
// and the boot self-test has run. Requests made before this wait in the
// queue. If the self-test failed, every request ends in Error.
void data_start() noexcept;  // spawns the workers once
// Once per frame on the main thread, before the screens draw: moves finished
// results into the slots, and empties the follows' slots once their user is
// no longer the one logged in. Cheap when nothing has finished.
void data_frame() noexcept;  // main thread: move finished results into the slots

}  // namespace evo_twitch
