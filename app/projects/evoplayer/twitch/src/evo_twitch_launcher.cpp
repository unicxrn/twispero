// Twitch launcher: finds a stream on a worker thread and plays it through
// EVO's PlaybackController, starting it on a second worker.
//
// Three ways in, one path:
//   evo_twitch_play(login)      a screen asked for a channel (no fallback)
//   evo_twitch_launcher_start() the boot autoplay, once per launch and only
//                               when <data root>/twitch_autoplay exists
//   evo_twitch_switch_quality() the player's quality picker (R2): the same
//                               channel again, in another quality
//
//   main thread                           resolve worker
//   play / launcher_start --spawn-------> autoplay: wait for /data, look for
//     / switch_quality                      the file; off -> "nothing"
//                                         channel (play request, switch or
//                                           the channel file) or the top
//                                           streams, token, usher master
//                                           playlist, the quality asked for
//                                           (Settings, or the picker's row)
//   evo_twitch_launcher_pump() <-mailbox- progress note, then url + title +
//                                          the quality list, the error, or
//                                          nothing
//     still fine to start? (see onResolved / onQualityResolved)
//       a switch: off the player (returnFromPlayback), then on the next
//         pump call stopPlayback() (pumpLeaving)
//       start worker: startPlaybackSource()
//       navigateTo(ScreenId::Player)
//
// Phases (main thread):
//   Idle --play / launcher_start / switch--> Resolving --stream--> Starting --> Idle
//                                                |  \--switch--> Leaving --/
//                                                |  error, nothing, or the user moved
//                                                +--------------------------------> Idle
//
// A quality switch (Kind::Quality) keeps the old quality playing while the
// worker looks the stream up again (variant addresses are signed, so the list
// from the first lookup can't be reused). When the new address is in hand,
// and the user is still in the player or the quality picker (an exit dialog
// they opened wins: "Quality not changed"), the main thread moves off
// ScreenId::Player (Leaving).
// On the next pump call, so a frame without the video has been drawn and the
// player's GPU mode is off, as when the exit dialog stops a stream, it stops
// the old quality and launches the start worker: the frame loop and
// PlayerScreen read play_fmt, g_pp_pb and the controller with no lock, so
// nothing may draw the player while the start worker opens the stream.
// onStarted() opens the player again. Both steps check that the launcher's
// own stream of that channel still plays, that the app isn't quitting and
// that no play request waits; the switch leaves otherPlayback() out (a
// playback screen is up, and that's the point). A lookup that lands on the
// quality already playing (the one asked for is gone) only refreshes the
// list. A play request that comes in before the start wins: the switch is
// dropped (stopping the old quality if the player was already left) and the
// request goes next, as with the autoplay check. While the new stream starts,
// evo_twitch_launcher_switching_channel() keeps chat on the channel.
// One lookup or start at a time: a play request while one is in flight is
// turned down ("Already starting a stream"). The exceptions are the autoplay
// check before it has decided to play and a quality switch's lookup: a play
// request stands them down (the autoplay's g_auto_gate, one compare-and-swap
// decides) and goes next. Back in Idle, the next play request works as the
// first did.
//
// The autoplay only plays while the user is still on the screen it started
// from with nothing playing; anything they did themselves wins. A play
// request goes ahead from any screen outside the player. What an error or a
// finished file left in the controller is stopped on the main thread first;
// anything else there means another source owns the player, and the request
// is turned down with a toast.
//
// This is the only starter that opens a stream on a worker. The dev remote's
// `play` starts one on the main thread and is refused while busy() is 1. A
// play request, at the request and again before its start, refuses while a
// playback screen is up and while the controller is Opening (any other start
// in flight). See otherPlayback().
//
// PlaybackController has no locking, so nothing on the main thread may start
// or stop playback while the start worker is inside startPlaybackSource().
// evo_twitch_launcher_busy() says when that is, and Application uses it to:
//   - hold the soft close's first stage (the one that calls stopPlayback()),
//   - keep pad input away from the screens, so no tile starts something else
//     (not while it is Home's picture that opens: a play request then
//     waits, see request()),
//   - make shutdown() wait (up to 40 s) before it stops playback.
// When the worker returns it swaps g_start_owner Running -> Reported; from then
// on it never touches the controller again, so busy() drops and the main thread
// owns the result. If the user asked to quit meanwhile, the next frame runs the
// soft close and this pump in one of two orders, and both end stopped:
//   - soft close first: its stopPlayback() runs now that the worker is done,
//     then onStarted() joins the worker, sees the quit and stops again, which
//     finds nothing playing; it does not open the player;
//   - pump first: onStarted() joins the worker, sees the quit and stops the
//     stream on the main thread instead of opening the player; the soft
//     close's own stopPlayback() then finds nothing playing.
// While busy, the pump re-shows "Still starting <name>..." every 3 s, so a
// held quit or a dead pad does not look like a hang.
//
// Defensive fallback, unreachable while Application honours busy(): if the
// soft close ever got past its first stage (the safe-to-close screen) with the
// worker still running, the pump swaps Running -> Abandoned and detaches it.
// The worker then finds its own swap refused and stops the playback itself,
// because the main thread parks for good ~150 frames later and never pumps
// again. One compare-and-swap decides, so exactly one side ever stops it.
//
// Videos and clips (stage 7) take the same path as a play request:
//   evo_twitch_play_video(id)   a past broadcast, highlight or upload: its
//                               token and usher playlist (twitch::resolveVod),
//                               the quality asked for, the qualities it has
//   evo_twitch_play_clip(slug)  a clip: its signed files (twitch::clipVariants),
//                               the closest one to the quality asked for
// with the same one-at-a-time rule, the same stand-downs and the same checks
// before the start. The source they start is "twitch-vod" or "twitch-clip",
// not live, so EVO's timeline and seek work and chat (which follows only
// "twitch") stays closed. A video starts where it was left
// (evo_twitch_progress.h), and a quality switch of a video or clip carries
// on from the position it had when the player was left: both through
// startPlaybackSource()'s resume offset (a paused one is paused again once
// its first picture is up, see pauseTick()). While
// a video plays, the pump notes its position every frame and saves it every
// 15 s, and once more on the frame it is seen to have stopped, however it
// stopped (the exit dialog, an error); a quality switch saves it before it
// stops it, and the app's soft close and SIGTERM shutdown save it on the main
// thread before they stop playback (evo_twitch_launcher_save_progress). A
// broadcast still being recorded has no length yet: the player treats it as
// live, it saves nothing, and its quality doesn't switch.
//
// Home's live picture (Kind::Preview, evo_twitch_preview()) takes the same
// path as a play request, with these differences:
//   - it asks for kPreviewQuality, not Settings' quality, starts quiet
//     (PlaybackSource::quiet: no toasts) with the sound silent, and never
//     opens the player or joins chat;
//   - it is the one start that leaves the pad alive: a play request while
//     it starts waits (g_pending) and goes once the start worker is back,
//     after the picture is stopped on the main thread;
//   - every frame, previewTick() drops a picture that died (an error, the
//     end of the stream, a decoder that gave up, no picture after
//     kPreviewNoPictureMs) by stopping it on the main thread;
//   - a lookup or start that fails, or a picture that dies, holds the next
//     try back (kPreviewRetryMs, doubling up to kPreviewRetryMaxMs) and
//     leaves that channel alone for kPreviewSkipMs;
//   - X on the hero takes the picture over (evo_twitch_preview_take()) only
//     when it already plays in the quality Settings would pick; otherwise
//     the hero plays like any other card, and the picture stops first.
//
// Every piece of state here is file scope and trivially constructible (EVO
// keeps static initialization trivial). The one non-trivial object, the
// PlaybackSource, is handed to the start worker as its argument and freed by
// whichever side owns the result.

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <exception>
#include <string>
#include <utility>
#include <vector>

#include "evo/Application.hpp"
#include "evo/screens/ScreenManager.hpp"
#include "evo_boot_log.h"
#include "evo_data_path.h"
#include "evo_jailbreak.h"
#include "evo_toast.h"
#include "evo_twitch.h"
#include "evo_twitch_chat_mode.h"
#include "evo_twitch_history.h"
#include "evo_twitch_internal.h"
#include "evo_twitch_progress.h"
#include "evo_twitch_settings.h"
#include "evo_twitch_text.h"
#include "twitch/browse.hpp"
#include "twitch/error.hpp"
#include "twitch/gql.hpp"
#include "twitch/login.hpp"
#include "twitch/playback.hpp"
#include "twitch/progress.hpp"
#include "twitch/streams.hpp"
#include "twitch/videos.hpp"

/* Declared here rather than via the media headers: those pull in ffmpeg,
 * and this file only needs the three switches for Home's muted picture and
 * the two signs that it died. */
extern "C" {
void evo_audio_set_silent(int on);
void evo_pb_set_presenting(int on);
void evo_pb_set_quiet(int on);
int evo_pb_is_eof(void);
int evo_pb_decode_fatal(void);
}

namespace {

constexpr size_t kStackBytes = 2 * 1024 * 1024;
constexpr const char* kChannelFile = "twitch_channel.txt";
constexpr const char* kAutoplayFile = "twitch_autoplay";
constexpr const char* kEllipsis = "\xE2\x80\xA6";
constexpr const char* kMiddleDot = " \xC2\xB7 ";  // between the name and the title
constexpr int kTopCandidates = 5;
// Toasts for a start that another source stands in the way of.
constexpr const char* kOtherPlaying = "Something else is playing";
constexpr const char* kOtherStarting = "Something else is starting";
constexpr const char* kSelftestFailed = "Self-test failed, see evo.log";
// A play request names one channel, so "Not found" means that one.
constexpr const char* kChannelNotFound = "Channel not found";
// And for a video or clip, that one is gone (deleted, or never was).
constexpr const char* kVideoGone = "This video is gone";
constexpr const char* kClipGone = "This clip is gone";
// What PlaybackSource::provider says for each kind of stream. Chat and the
// side-by-side layout follow "twitch" only (evo_twitch_chat_mode.h).
constexpr const char* kProviderLive = "twitch";
constexpr const char* kProviderVideo = "twitch-vod";
constexpr const char* kProviderClip = "twitch-clip";
// A video's position goes to its progress this often while it plays.
constexpr long long kProgressSaveMs = 15000;
// Home's live picture: small (an 800x450 box), so the closest rung at or
// below 480p30. After a failure the next try waits kPreviewRetryMs, twice
// that after a second one in a row, and so on up to kPreviewRetryMaxMs; the
// channel that failed is left alone for kPreviewSkipMs. A picture that has
// started but shows nothing for kPreviewNoPictureMs counts as failed.
constexpr const char* kPreviewQuality = "480p30";
constexpr const char* kPreviewClosing = "Closing once the live preview has stopped\xE2\x80\xA6";
constexpr long long kPreviewRetryMs = 15000;
constexpr long long kPreviewRetryMaxMs = 5 * 60 * 1000;
constexpr long long kPreviewSkipMs = 10 * 60 * 1000;
constexpr long long kPreviewNoPictureMs = 20000;

// ---- Mailbox: written by the resolve worker, read by the pump ----
enum Outcome : int { kOutNothing = 0, kOutStream = 1, kOutError = 2 };
struct Mailbox {
  char note[160];   // progress for a toast ("Looking for a live stream...")
  int outcome;      // kOutStream: url/title/name/item and the qualities are set;
                    // kOutError: error/user are set; kOutNothing: the autoplay is
                    // off or stood down for a play request
  char url[4096];   // the chosen variant's media playlist (FFmpeg's URL limit)
  char title[512];  // what the player shows ("" for a quality switch, which keeps its own)
  char name[96];    // display name, for "Starting <name>..." (likewise)
  char login[32];   // the channel's login; "" for a video or clip whose channel isn't known
  char item[96];    // what plays: the login, a video's id or a clip's slug
  char error[256];  // the exception's own words (log, autoplay toast)
  char user[64];    // the short sentence for a play request's toast
  evo_twitch_quality_t qualities[EVO_TWITCH_QUALITY_MAX];  // twitch::qualityMenu()
  int quality_count;
  int quality_chosen;  // the chosen variant's row, -1 if it has none
  int quality_wanted;  // Kind::Preview: the row Settings' quality would pick, -1 otherwise
  evo_twitch_now_playing_t info;  // the channel, for the OSD (empty for a quality switch)
};
pthread_mutex_t g_box_mx = PTHREAD_MUTEX_INITIALIZER;
Mailbox g_box;
std::atomic<bool> g_note_ready{false};
std::atomic<bool> g_resolve_done{false};

// ---- What plays ----
enum class Media : int { Live = 0, Video = 1, Clip = 2 };

// What a video's or clip's card said of it (evo_twitch_video_info_t, copied).
struct VideoMeta {
  int type;  // EVO_TWITCH_VIDEO_*
  char title[512];
  char login[32];
  char name[96];
  char game[128];
  char age[48];
  char published[40];
  char curator[96];
  char thumb[512];
  char avatar[512];
  int seconds;
  int views;
};

// ---- What the resolve worker is asked to do ----
// Written by the main thread before it spawns the worker (pthread_create
// orders that), read by the worker; never while a worker runs.
enum class Kind { Auto, Play, Quality, Video, Clip, Preview };
Kind g_req_kind = Kind::Auto;
char g_req_id[96];       // Play: the normalized login; Video: the id; Clip: the slug;
                         // Quality: the item playing
Media g_req_media = Media::Live;  // Kind::Quality: what is being switched
VideoMeta g_req_meta;    // Kind::Video and Kind::Clip: the card's details
char g_req_quality[32];  // twitch::pickQuality(): Settings' default, or the picker's row
char g_req_wanted[32];   // Kind::Preview: Settings' default, to tell whether the picture is in it

// The autoplay check's decision, between the worker (Deciding -> On) and a
// play request on the main thread (Deciding -> StoodDown). One
// compare-and-swap decides, so the worker never plays after a play request
// got in, and a play request never waits behind an autoplay that is playing.
enum AutoGate : int { kGateDeciding = 0, kGateOn = 1, kGateStoodDown = 2 };
std::atomic<int> g_auto_gate{kGateDeciding};

// ---- Start worker hand-off (see the top of the file) ----
enum StartOwner : int { kRunning = 0, kReported = 1, kAbandoned = 2 };
std::atomic<int> g_start_owner{kRunning};
std::atomic<bool> g_start_ok{false};

// The start worker's argument: what to open and where to start in it.
struct StartJob {
  evo::PlaybackSource src;
  double resume = 0.0;  // seconds into a video or clip; 0 from the top (or live)
};

// A play request: what it asks for. Kept while it waits for the autoplay
// check or a quality switch to stand down.
struct Request {
  Kind kind;      // Play, Video or Clip
  char id[96];    // the normalized login, the video's id or the clip's slug
  VideoMeta meta; // Video and Clip
};

// ---- Main-thread state ----
// Leaving: a quality switch has left the player and stops and starts on the
// next pump call (onQualityResolved(), pumpLeaving()).
enum class Phase { Idle, Resolving, Leaving, Starting };
Phase g_phase = Phase::Idle;
Kind g_kind = Kind::Auto;  // what the current Resolving/Leaving/Starting is for
// Home's muted live picture. Main thread only. While it is up, a real play
// request stops it and goes ahead; it never counts as "something else".
bool g_home_preview = false;
bool g_preview_cancel = false;  // leave Home, or a play request, during the lookup or start
// The picture plays in the quality Settings would pick, so X can take it over.
bool g_preview_full = false;
// Since when it has been up, and whether its first picture has shown yet.
long long g_preview_up_ms = 0;
bool g_preview_seen = false;
// Failures in a row, when the next try may come, and the channel that failed
// last with when it may be tried again (see kPreviewRetryMs).
int g_preview_failures = 0;
long long g_preview_wait_until_ms = 0;
char g_preview_failed[96];
long long g_preview_failed_until_ms = 0;
bool g_in_pump = false;
bool g_autoplay_tried = false;    // evo_twitch_launcher_start() ran (once per launch)
evo::ScreenId g_auto_screen = evo::ScreenId::TwitchHome;  // where the autoplay started
bool g_pending = false;           // a play request waits for the autoplay check to stand down
Request g_pending_req;
pthread_t g_resolve_thread;
pthread_t g_start_thread;
StartJob* g_start_job = nullptr;  // main's copy, valid until Reported/Abandoned
char g_start_still[160];         // "Still starting <name>..." / "Still switching to <quality>..."
long long g_start_t0_ms = 0;     // when the start worker was launched
long long g_start_toast_ms = 0;  // last progress toast
bool g_start_slow_logged = false;

// A stream of the launcher's: the one playing (g_playing, from onStarted()
// until the next one starts) and the one starting (g_starting).
struct Stream {
  Media media;
  char item[96];    // PlaybackSource::item_id: the login, a video's id, a clip's slug
  char login[32];   // the channel's login ("" for a video or clip whose channel isn't known)
  char title[512];  // the player's title
  char name[96];    // for the toasts
  evo_twitch_quality_t qualities[EVO_TWITCH_QUALITY_MAX];
  int quality_count;
  int quality_current;  // the playing row, -1 if none
  evo_twitch_now_playing_t info;  // evo_twitch_now_playing()
  VideoMeta meta;   // a video or clip: its card's details (the progress keeps some)
  double resume;    // where it was started, seconds; 0 from the top
  bool resumed;     // a video picked up from its saved progress ("Resumed at ...")
  bool pause_after; // a quality switch of a paused stream: paused again once its
                    // first picture is up (pauseTick())
};
Stream g_playing;
Stream g_starting;
char g_switch_label[48];  // Kind::Quality: the row asked for, for the toasts
Mailbox g_switch_box;     // Phase::Leaving: the new quality, between the two steps

// The video whose position the pump follows (progressTick()): while one of
// ours plays, its last position and length are noted every frame, and saved
// every kProgressSaveMs and once it has stopped.
struct Watching {
  bool active;
  char id[96];
  VideoMeta meta;
  double position;    // seconds, the last read while it played
  double duration;    // seconds, 0 while the player doesn't know
  double saved;       // the position last saved, -1 none
  long long saved_ms; // when it was last saved (or started to be followed)
};
Watching g_watch;
bool recordWatched(Watching& w);  // below, with progressTick()

bool spawn(pthread_t* thread, void* (*fn)(void*), void* arg) {
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, kStackBytes);
  const int rc = pthread_create(thread, &attr, fn, arg);
  pthread_attr_destroy(&attr);
  if (rc != 0) evo_log_error("twitch: pthread_create rc=%d", rc);
  return rc == 0;
}

// Copies at most cap-1 bytes and never cuts a UTF-8 character in half.
void copyUtf8(char* dst, size_t cap, const char* src, size_t len) {
  if (cap == 0) return;
  size_t n = len;
  if (n >= cap) {
    n = cap - 1;
    while (n > 0 && (static_cast<unsigned char>(src[n]) & 0xC0) == 0x80) --n;
  }
  memcpy(dst, src, n);
  dst[n] = '\0';
}
void copyUtf8(char* dst, size_t cap, const std::string& src) {
  copyUtf8(dst, cap, src.data(), src.size());
}
void copyUtf8(char* dst, size_t cap, const char* src) { copyUtf8(dst, cap, src, strlen(src)); }

// The launcher's toasts are one line: what is happening ("Starting
// NordlichtTV..."), or what went wrong, in red ("No connection").
void showToast(const char* message) { toast(message, nullptr); }
void showError(const char* message) { toast_error(message, nullptr); }

long long monoMs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<long long>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// The soft close is past its first stage and showing its last screen. With the
// start worker running that only happens if busy() was ignored (see the top).
bool softClosePastFirstStage() {
  evo::ScreenManager* sm = evo::Application::getInstance().getScreenManager();
  return sm && sm->getCurrentScreenId() == evo::ScreenId::SafeToClose;
}

// The user asked to quit: a soft close is pending (perhaps held by busy()) or
// already under way.
bool appQuitting() {
  return evo::Application::getInstance().isSoftClosing() || softClosePastFirstStage();
}

// The boot self-test passed (running it now if it has not run yet).
bool selftestPassed() {
  int state = evoTwitchSelftestState();
  if (state < 0) state = evo_twitch_selftest();
  return state == 1;
}

// The player's title: "<name> · <stream title>", or just the name. Drawn in
// the display face, which has no Greek (evo_twitch_text.h): characters the
// fonts lack are left out, and a name with nothing left becomes the login.
std::string playerTitle(const std::string& name, const std::string& title, const std::string& login) {
  const std::string shown = evo_twitch_text::channelName(name, login, false);
  const std::string what = evo_twitch_text::drawable(title, false);
  return what.empty() ? shown : shown + kMiddleDot + what;
}

// A channel's name for the toasts ("Starting <name>…"), the same way.
std::string toastName(const std::string& name, const std::string& login) {
  return evo_twitch_text::channelName(name, login, true);
}

void dropHomePreview();

// Home's picture: no sound, decode while the player screen isn't up, and the
// media pipeline's toasts in the log only. All three on before it opens and
// off once it is dropped or taken over.
void previewSwitches(bool on) {
  evo_audio_set_silent(on ? 1 : 0);
  evo_pb_set_presenting(on ? 1 : 0);
  evo_pb_set_quiet(on ? 1 : 0);
}

// Another source owns the player or is starting a stream of its own (see the
// top of the file): the toast for turning a play request down, or nullptr
// when it may go ahead. What an error or a finished file left behind does not
// count; the caller stops it before starting.
const char* otherPlayback(evo::IPlaybackController* pb, evo::ScreenManager* sm) {
  // Home's muted picture is stopped by the caller before a real play.
  if (g_home_preview) return nullptr;
  if (sm->isPlaybackScreen(sm->getCurrentScreenId())) return kOtherPlaying;
  switch (pb->getPlaybackState()) {
    case evo::PlaybackState::Stopped:
    case evo::PlaybackState::Error:
    case evo::PlaybackState::Finished:
      return nullptr;
    case evo::PlaybackState::Opening:
      return kOtherStarting;
    default:
      return kOtherPlaying;
  }
}

const char* providerOf(Media media) {
  switch (media) {
    case Media::Video: return kProviderVideo;
    case Media::Clip: return kProviderClip;
    default: return kProviderLive;
  }
}

// The launcher's own stream is playing (or paused): the controller holds the
// Twitch stream, video or clip in g_playing. Main thread only, never while
// busy().
bool ownStreamPlaying(evo::IPlaybackController* pb) {
  if (!pb || g_playing.item[0] == '\0') return false;
  switch (pb->getPlaybackState()) {
    case evo::PlaybackState::Playing:
    case evo::PlaybackState::Paused:
    case evo::PlaybackState::Scrubbing:
    case evo::PlaybackState::Seeking:
      break;
    default:
      return false;
  }
  const evo::PlaybackSource* src = pb->currentSource();
  return src && src->provider == providerOf(g_playing.media) && src->item_id == g_playing.item;
}

// What a play request of this kind starts.
Media mediaOf(Kind kind) {
  return kind == Kind::Video ? Media::Video : kind == Kind::Clip ? Media::Clip : Media::Live;
}

// "the video", "the clip", for the toasts.
const char* mediaWord(Media media) { return media == Media::Clip ? "the clip" : "the video"; }

// ---- Resolve worker ----

void postNote(const std::string& note) {
  pthread_mutex_lock(&g_box_mx);
  copyUtf8(g_box.note, sizeof g_box.note, note);
  pthread_mutex_unlock(&g_box_mx);
  g_note_ready = true;
}

// The stream found: its address, the quality chosen and every quality it has.
struct Resolved {
  twitch::Variant chosen;
  std::vector<twitch::QualityOption> menu;
  std::string wanted;  // Kind::Preview: the variant Settings' quality would pick
};

// What the OSD shows of a channel: login and display name, and what Twitch
// said of its stream (viewers -1 and the rest "" when it said nothing).
evo_twitch_now_playing_t channelInfo(const std::string& login, const std::string& name,
                                     const std::string& title, const std::string& game, int viewers,
                                     const std::string& avatarUrl) {
  evo_twitch_now_playing_t info;
  memset(&info, 0, sizeof info);
  copyUtf8(info.login, sizeof info.login, login);
  copyUtf8(info.name, sizeof info.name, name.empty() ? login : name);
  copyUtf8(info.title, sizeof info.title, title);
  copyUtf8(info.game, sizeof info.game, game);
  info.viewers = viewers;
  copyUtf8(info.avatar_url, sizeof info.avatar_url, avatarUrl);
  info.stamp = static_cast<unsigned long long>(monoMs());  // data_now_ms()'s clock
  info.video_type = 0;  // a live stream (videoInfo() says otherwise)
  info.views = -1;
  return info;
}

void postStream(const Resolved& r, const std::string& title, const std::string& name,
                const std::string& login, const std::string& item, const evo_twitch_now_playing_t& info) {
  pthread_mutex_lock(&g_box_mx);
  g_box.outcome = kOutStream;
  g_box.info = info;
  copyUtf8(g_box.url, sizeof g_box.url, r.chosen.uri);
  copyUtf8(g_box.title, sizeof g_box.title, title);
  copyUtf8(g_box.name, sizeof g_box.name, name);
  copyUtf8(g_box.login, sizeof g_box.login, login);
  copyUtf8(g_box.item, sizeof g_box.item, item);
  g_box.quality_count = 0;
  g_box.quality_chosen = -1;
  g_box.quality_wanted = -1;
  for (const twitch::QualityOption& q : r.menu) {
    if (g_box.quality_count >= EVO_TWITCH_QUALITY_MAX) break;
    evo_twitch_quality_t& row = g_box.qualities[g_box.quality_count];
    copyUtf8(row.name, sizeof row.name, q.name);
    copyUtf8(row.label, sizeof row.label, q.label);
    row.width = q.width;
    row.height = q.height;
    row.fps = q.fps;
    if (q.name == r.chosen.name) g_box.quality_chosen = g_box.quality_count;
    if (!r.wanted.empty() && q.name == r.wanted) g_box.quality_wanted = g_box.quality_count;
    ++g_box.quality_count;
  }
  pthread_mutex_unlock(&g_box_mx);
  g_resolve_done = true;
}

void postError(const char* why, const char* user) {
  pthread_mutex_lock(&g_box_mx);
  g_box.outcome = kOutError;
  copyUtf8(g_box.error, sizeof g_box.error, why && why[0] ? why : "Unexpected error");
  copyUtf8(g_box.user, sizeof g_box.user, user && user[0] ? user : kEvoTwitchFailed);
  pthread_mutex_unlock(&g_box_mx);
  g_resolve_done = true;
}

void postNothing() {
  pthread_mutex_lock(&g_box_mx);
  g_box.outcome = kOutNothing;
  pthread_mutex_unlock(&g_box_mx);
  g_resolve_done = true;
}

// The data root is /data/ps5twitch only once the jailbreak daemon has lifted
// the sandbox, which can land a while after boot. Before that the autoplay
// switch and the channel file are out of reach. Returns early when a play
// request stands the autoplay down.
void waitForDataRoot() {
  for (int i = 0; i < 100 && !evo_jailbreak_is_open() && g_auto_gate.load() == kGateDeciding; ++i)
    usleep(100 * 1000);
  if (!evo_jailbreak_is_open() && g_auto_gate.load() == kGateDeciding)
    evo_log_warn("twitch: data folder still closed after 10 s, %s and %s cannot be read",
                 kAutoplayFile, kChannelFile);
}

// The autoplay switch: <data root>/twitch_autoplay exists (any content).
bool autoplayFileExists() {
  FILE* f = fopen(evo_data_path(kAutoplayFile), "r");
  if (!f) return false;
  fclose(f);
  return true;
}

// First line of <data root>/twitch_channel.txt, trimmed. Empty when the file
// is missing or blank.
std::string readChannelFile() {
  char line[128];
  FILE* f = fopen(evo_data_path(kChannelFile), "r");
  if (!f) return std::string();
  if (!fgets(line, sizeof line, f)) line[0] = '\0';
  fclose(f);

  const char* begin = line;
  if (strncmp(begin, "\xEF\xBB\xBF", 3) == 0) begin += 3;  // UTF-8 BOM (Notepad)
  while (*begin == ' ' || *begin == '\t') ++begin;
  size_t len = strlen(begin);
  while (len > 0 && (begin[len - 1] == ' ' || begin[len - 1] == '\t' ||
                     begin[len - 1] == '\r' || begin[len - 1] == '\n'))
    --len;
  return std::string(begin, len);
}

// Usher's "p" parameter: any number; the web player sends up to 7 digits.
unsigned usherNonce() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  const unsigned mixed =
      static_cast<unsigned>(ts.tv_nsec) ^ (static_cast<unsigned>(ts.tv_sec) * 2654435761u);
  return mixed % 10000000u;
}

// Token, master playlist and the quality asked for (g_req_quality) for one
// channel, with the list of the qualities it has.
Resolved resolveStream(twitch::GqlClient& gql, const std::string& login) {
  const twitch::AccessToken token = twitch::fetchStreamToken(gql, login, "embed");
  evo_log("twitch: %s: got a playback token", login.c_str());
  const std::string master = twitch::usherStreamUrl(login, token, usherNonce());
  const std::vector<twitch::Variant> variants = twitch::fetchVariants(evoTwitchHttp(), master);
  evo_log("twitch: %s: %d qualities", login.c_str(), static_cast<int>(variants.size()));
  Resolved r;
  r.chosen = twitch::pickQuality(variants, g_req_quality);
  r.menu = twitch::qualityMenu(variants);
  if (g_req_wanted[0]) r.wanted = twitch::pickQuality(variants, g_req_wanted).name;
  evo_log("twitch: picked %s (%s) %dx%d for \"%s\"", r.chosen.label.c_str(), r.chosen.name.c_str(),
          r.chosen.width, r.chosen.height, g_req_quality);
  if (r.chosen.uri.empty() || r.chosen.uri.size() >= sizeof g_box.url)
    throw twitch::ApiError("the stream address is empty or too long");
  return r;
}

// A video's playlist and qualities, the one asked for (g_req_quality)
// chosen. The same master playlist shape as a live channel's.
Resolved resolveVideoStream(twitch::GqlClient& gql, const std::string& id) {
  const std::vector<twitch::Variant> variants = twitch::resolveVod(gql, evoTwitchHttp(), id);
  evo_log("twitch: video %s: %d qualities", id.c_str(), static_cast<int>(variants.size()));
  Resolved r;
  r.chosen = twitch::pickQuality(variants, g_req_quality);
  r.menu = twitch::qualityMenu(variants);
  evo_log("twitch: picked %s (%s) %dx%d for \"%s\"", r.chosen.label.c_str(), r.chosen.name.c_str(),
          r.chosen.width, r.chosen.height, g_req_quality);
  if (r.chosen.uri.empty() || r.chosen.uri.size() >= sizeof g_box.url)
    throw twitch::ApiError("the video address is empty or too long");
  return r;
}

// "1080p60", "720p": a clip's file the way the picker names a stream's.
std::string clipLabel(const twitch::ClipVariant& v) {
  return v.quality + "p" + (v.fps >= 45 ? "60" : "");
}

// A clip's files and the one closest to the quality asked for, as a stream's
// variants: the picker lists them best first, by the clip's own quality
// names ("1080", "720") so a switch can ask for one again.
Resolved resolveClipStream(twitch::GqlClient& gql, const std::string& slug) {
  std::vector<twitch::ClipVariant> files = twitch::clipVariants(gql, slug);
  if (files.empty()) throw twitch::NotFoundError("clip has no files: " + slug);
  evo_log("twitch: clip %s: %d qualities", slug.c_str(), static_cast<int>(files.size()));
  const twitch::ClipVariant chosen = twitch::pickClipQuality(files, g_req_quality);
  std::stable_sort(files.begin(), files.end(), [](const twitch::ClipVariant& a, const twitch::ClipVariant& b) {
    const int ha = atoi(a.quality.c_str()), hb = atoi(b.quality.c_str());
    return ha != hb ? ha > hb : a.fps > b.fps;
  });
  Resolved r;
  for (const twitch::ClipVariant& v : files) {
    twitch::QualityOption q;
    q.name = v.quality;
    q.label = clipLabel(v);
    q.height = atoi(v.quality.c_str());
    q.width = q.height * 16 / 9;  // Twitch's clips are 16:9 ("landscape")
    q.fps = v.fps;
    r.menu.push_back(std::move(q));
  }
  r.chosen.name = chosen.quality;
  r.chosen.label = clipLabel(chosen);
  r.chosen.height = atoi(chosen.quality.c_str());
  r.chosen.width = r.chosen.height * 16 / 9;
  r.chosen.fps = chosen.fps;
  r.chosen.uri = chosen.url;
  evo_log("twitch: picked the %s file for \"%s\"", r.chosen.label.c_str(), g_req_quality);
  if (r.chosen.uri.empty() || r.chosen.uri.size() >= sizeof g_box.url)
    throw twitch::ApiError("the clip address is empty or too long");
  return r;
}

// A quality switch: the same channel, video or clip again, in the quality the
// picker named. The player keeps its title and details, so none are fetched.
void resolveQuality(twitch::GqlClient& gql, const std::string& item) {
  evo_log("twitch: %s: switching to %s", item.c_str(), g_req_quality);
  const evo_twitch_now_playing_t none = channelInfo("", "", "", "", -1, "");
  if (g_req_media == Media::Video)
    postStream(resolveVideoStream(gql, item), "", "", "", item, none);
  else if (g_req_media == Media::Clip)
    postStream(resolveClipStream(gql, item), "", "", "", item, none);
  else
    postStream(resolveStream(gql, item), "", "", item, item, channelInfo(item, "", "", "", -1, ""));
}

// What the OSD shows of a video or clip: its channel (when the card knew it),
// its title and the card's line.
evo_twitch_now_playing_t videoInfo(const VideoMeta& m) {
  evo_twitch_now_playing_t info = channelInfo(m.login, m.name[0] ? m.name : m.login, m.title, m.game, -1, m.avatar);
  info.video_type = m.type;
  copyUtf8(info.age, sizeof info.age, m.age);
  copyUtf8(info.curator, sizeof info.curator, m.curator);
  info.views = m.views;
  return info;
}

// The player's title for a video or clip: "<channel> · <title>" like a
// stream's, or the title alone when the channel isn't known.
std::string videoTitle(const VideoMeta& m) {
  if (m.name[0] || m.login[0]) return playerTitle(m.name, m.title, m.login);
  return evo_twitch_text::drawable(m.title, false);
}

// A video (Kind::Video) or a clip (Kind::Clip): that one or an error.
void resolveVideo(twitch::GqlClient& gql, const std::string& id, Media media) {
  const VideoMeta& m = g_req_meta;
  evo_log("twitch: resolving %s %s", media == Media::Clip ? "clip" : "video", id.c_str());
  postNote(std::string("Opening ") + mediaWord(media) + kEllipsis);
  const Resolved r = media == Media::Clip ? resolveClipStream(gql, id) : resolveVideoStream(gql, id);
  postStream(r, videoTitle(m), mediaWord(media), m.login, id, videoInfo(m));
}

// A play request: that channel or an error, never another one.
void resolvePlay(twitch::GqlClient& gql, const std::string& login) {
  evo_log("twitch: resolving %s (play request)", login.c_str());
  postNote("Opening " + login + kEllipsis);

  // The channel's details for the player (name, stream title, category,
  // viewers, avatar), and a quick offline check before the token. If Twitch
  // answers this one oddly, the token and usher calls below still decide.
  std::string name = login;
  std::string title, game, avatar;
  int viewers = -1;
  try {
    const twitch::ChannelInfo info = twitch::fetchChannel(gql, login);
    if (!info.displayName.empty()) name = info.displayName;
    avatar = info.profileImageUrl;
    if (!info.live) throw twitch::OfflineError(login + " is offline");
    title = info.streamTitle;
    game = info.gameName;
    viewers = info.viewers;
  } catch (const twitch::ApiError& e) {
    evo_log_warn("twitch: %s: no channel details (%s), going on", login.c_str(), e.what());
  }

  postStream(resolveStream(gql, login), playerTitle(name, title, login), toastName(name, login), login, login,
             channelInfo(login, name, title, game, viewers, avatar));
}

// The boot autoplay: off unless the switch file exists. Then the channel in
// the channel file, or the most-watched stream that actually opens.
void resolveAuto(twitch::GqlClient& gql) {
  waitForDataRoot();
  if (g_auto_gate.load() == kGateStoodDown) {
    evo_log("twitch: autoplay check skipped, a play request came first");
    postNothing();
    return;
  }
  if (!autoplayFileExists()) {
    evo_log("twitch: autoplay off (no %s), waiting for a play request", kAutoplayFile);
    postNothing();
    return;
  }
  int deciding = kGateDeciding;
  if (!g_auto_gate.compare_exchange_strong(deciding, kGateOn)) {
    evo_log("twitch: autoplay on, but a play request came first");
    postNothing();
    return;
  }
  evo_log("twitch: autoplay on (%s found)", kAutoplayFile);

  // A channel named in the file: that one or nothing.
  const std::string wanted = readChannelFile();
  if (!wanted.empty()) {
    const std::string login = twitch::normalizeLogin(wanted);
    evo_log("twitch: resolving %s", login.c_str());
    postNote("Opening " + login + kEllipsis);
    postStream(resolveStream(gql, login), login, login, login, login, channelInfo(login, login, "", "", -1, ""));
    return;
  }

  // Otherwise the most-watched stream that actually opens. A channel that
  // vanished, went offline or was refused since the list was made is skipped;
  // a network failure stops the search, since the next one would fail too.
  evo_log("twitch: resolving the top live stream");
  postNote(std::string("Looking for a live stream") + kEllipsis);
  const std::vector<twitch::StreamInfo> top = twitch::topStreams(gql, kTopCandidates);
  if (top.empty()) throw twitch::NotFoundError("no live streams");
  std::string lastError;
  for (const twitch::StreamInfo& s : top) {
    try {
      const std::string login = twitch::normalizeLogin(s.login);
      evo_log("twitch: trying %s (%d viewers)", login.c_str(), s.viewers);
      const Resolved r = resolveStream(gql, login);
      const std::string& name = s.displayName.empty() ? login : s.displayName;
      postStream(r, playerTitle(name, s.title, login), toastName(name, login), login, login,
                 channelInfo(login, name, s.title, s.gameName, s.viewers, ""));
      return;
    } catch (const twitch::NotFoundError& e) {
      lastError = e.what();
    } catch (const twitch::OfflineError& e) {
      lastError = e.what();
    } catch (const twitch::HttpStatusError& e) {
      lastError = e.what();
    }
    evo_log_warn("twitch: skipping %s: %s", s.login.c_str(), lastError.c_str());
  }
  throw twitch::NotFoundError("none of the top streams would open (" + lastError + ")");
}

void* resolveWorker(void*) {
  // Before any core call; runs once per process (evo_twitch_internal.h).
  if (!evoTwitchWorkerCheck()) {
    postError("Self-test failed on a worker thread, see the log", kEvoTwitchFailed);
    return nullptr;
  }

  char why[256];
  const char* user = evoTwitchRun(
      [] {
        twitch::GqlClient gql(evoTwitchHttp());
        if (g_req_kind == Kind::Play || g_req_kind == Kind::Preview)
          resolvePlay(gql, g_req_id);
        else if (g_req_kind == Kind::Video || g_req_kind == Kind::Clip)
          resolveVideo(gql, g_req_id, mediaOf(g_req_kind));
        else if (g_req_kind == Kind::Quality)
          resolveQuality(gql, g_req_id);
        else
          resolveAuto(gql);
      },
      why, sizeof why);
  if (user) {
    evo_log_error("twitch: %s", why);
    postError(why, user);
  }
  return nullptr;
}

// ---- Start worker: startPlaybackSource() blocks on network I/O ----

void* startWorker(void* arg) {
  StartJob* job = static_cast<StartJob*>(arg);
  evo::IPlaybackController* pb = evo::Application::getInstance().getPlaybackController();
  bool ok = false;
  try {
    ok = pb && pb->startPlaybackSource(job->src, job->resume);
  } catch (...) {
    ok = false;
  }
  g_start_ok = ok;

  int expected = kRunning;
  if (g_start_owner.compare_exchange_strong(expected, kReported))
    return nullptr;  // the main thread owns the result (and the job) now

  // Abandoned: the app is closing and the main thread is parking for good.
  if (ok) {
    evo_log("twitch: the app closed while the stream opened, stopping it");
    try {
      pb->stopPlayback();
    } catch (...) {
    }
  }
  delete job;
  return nullptr;
}

// ---- Main thread ----

// Spawns the resolve worker for a play request, a video or clip, or a quality
// switch (id: the login, the video's id, the clip's slug, or the item
// playing) or the autoplay (nullptr), in `quality` (twitch::pickQuality()).
// Phase Idle -> Resolving; stays Idle if the thread won't start.
void beginResolve(Kind kind, const char* id, const char* quality) {
  g_req_kind = kind;
  snprintf(g_req_id, sizeof g_req_id, "%s", id ? id : "");
  snprintf(g_req_quality, sizeof g_req_quality, "%s", quality ? quality : "");
  snprintf(g_req_wanted, sizeof g_req_wanted, "%s",
           kind == Kind::Preview ? evo_twitch::settings().quality.c_str() : "");
  g_auto_gate = kGateDeciding;
  g_note_ready = false;
  g_resolve_done = false;
  if (!spawn(&g_resolve_thread, resolveWorker, nullptr)) {
    if (kind != Kind::Preview)
      showError(kind == Kind::Quality ? "Couldn't change the quality"
                : kind == Kind::Video ? "Couldn't start the video"
                : kind == Kind::Clip  ? "Couldn't start the clip"
                                      : "Couldn't start the stream");
    else
      evo_log_error("twitch: home preview: the lookup thread would not start");
    return;
  }
  g_kind = kind;
  g_phase = Phase::Resolving;
}

// A new stream, video or clip starts in the default quality from Settings
// (read here: the settings are main thread only). Home's picture is small.
void beginPlay(Kind kind, const char* id) {
  beginResolve(kind, id, kind == Kind::Preview ? kPreviewQuality : evo_twitch::settings().quality.c_str());
}

// A play request of any kind: its details for the worker, then the lookup.
void beginRequest(const Request& req) {
  g_req_meta = req.meta;
  beginPlay(req.kind, req.id);
}

// The play request that stood the autoplay or a quality switch down goes now.
// Phase is Idle.
void startPending() {
  if (!g_pending) return;
  g_pending = false;
  if (appQuitting()) {
    evo_log("twitch: the app is quitting, dropping the play request for %s", g_pending_req.id);
    return;
  }
  evo_log("twitch: play request for %s", g_pending_req.id);
  beginRequest(g_pending_req);
}

// Hands the stream, video or clip in `box` to the start worker, under this
// title, toast name and channel details, `resume` seconds in; still_note is
// what the progress toast says while it starts. `meta`: a video's or clip's
// card (nullptr for a live stream). Phase Idle -> Starting; stays Idle (and
// says why) if the thread won't start.
bool launchStart(const Mailbox& box, Media media, const char* title, const char* name,
                 const evo_twitch_now_playing_t& info, const VideoMeta* meta, double resume,
                 const char* still_note) {
  StartJob* job = new StartJob();
  job->src.url = box.url;
  job->src.title = title;
  job->src.provider = providerOf(media);
  job->src.item_id = box.item;
  job->src.is_live = media == Media::Live;
  // A past broadcast of a stream that is still on: from its start, not its end.
  job->src.from_start = media == Media::Video;
  // Home's picture: a failure is logged, not toasted over Home.
  job->src.quiet = g_kind == Kind::Preview;
  job->resume = media == Media::Live ? 0.0 : resume;
  g_start_job = job;

  g_start_ok = false;
  g_start_owner = kRunning;
  // Before the stream opens, so the first samples are silent and decode is
  // not parked on the Home screen.
  if (g_kind == Kind::Preview) previewSwitches(true);
  if (!spawn(&g_start_thread, startWorker, g_start_job)) {
    delete g_start_job;
    g_start_job = nullptr;
    if (g_kind == Kind::Preview) {
      previewSwitches(false);
      evo_log_error("twitch: home preview: the start thread would not start");
    } else {
      showError("Playback failed");
    }
    return false;
  }

  // What plays once it has started.
  g_starting.media = media;
  copyUtf8(g_starting.item, sizeof g_starting.item, box.item);
  copyUtf8(g_starting.login, sizeof g_starting.login, box.login);
  copyUtf8(g_starting.title, sizeof g_starting.title, title);
  copyUtf8(g_starting.name, sizeof g_starting.name, name);
  g_starting.quality_count = box.quality_count;
  memcpy(g_starting.qualities, box.qualities, sizeof g_starting.qualities);
  g_starting.quality_current = box.quality_chosen;
  g_starting.info = info;
  if (meta)
    g_starting.meta = *meta;
  else
    memset(&g_starting.meta, 0, sizeof g_starting.meta);
  g_starting.resume = job->resume;
  g_starting.resumed = false;
  g_starting.pause_after = false;

  copyUtf8(g_start_still, sizeof g_start_still, still_note);
  g_start_t0_ms = g_start_toast_ms = monoMs();
  g_start_slow_logged = false;
  g_phase = Phase::Starting;
  return true;
}

// Whether the stream the worker found may start now; when not, logs and shows
// why. A play request first stops what an error or a finished file left in
// the controller (main thread; otherPlayback() has ruled out anything running).
bool okToStart(Kind kind, const Mailbox& box, evo::IPlaybackController* pb,
               evo::ScreenManager* sm) {
  const evo::ScreenId screen = sm->getCurrentScreenId();
  const evo::PlaybackState state = pb->getPlaybackState();
  if (kind == Kind::Auto) {
    // The user has moved on (another screen, or something they started
    // themselves): their choice wins over the automatic start.
    if (screen != g_auto_screen) {
      evo_log("twitch: the user moved on (screen %d), not starting %s", static_cast<int>(screen),
              box.item);
      showToast("Autoplay skipped");
      return false;
    }
    if (state != evo::PlaybackState::Stopped) {
      evo_log("twitch: the player is busy (state %d), not starting %s", static_cast<int>(state),
              box.item);
      showToast("Autoplay skipped, something else is playing");
      return false;
    }
    return true;
  }
  // A play request: the user asked for this channel, from whatever screen
  // they are on now, unless another source took the player meanwhile.
  // Home's picture, if it is still up, stops here so this stream can open.
  if (g_home_preview) dropHomePreview();
  if (const char* why = otherPlayback(pb, sm)) {
    evo_log("twitch: not starting %s: screen %d, player state %d", box.item,
            static_cast<int>(screen), static_cast<int>(state));
    showToast(why);
    return false;
  }
  if (state == evo::PlaybackState::Error || state == evo::PlaybackState::Finished) {
    evo_log("twitch: stopping what the player holds (state %d) for %s", static_cast<int>(state),
            box.item);
    pb->stopPlayback();
  }
  return true;
}

// The toast for a lookup that failed: the autoplay is a test switch and keeps
// the exact words; a play request or a quality switch gets the short sentence
// (the log has the rest). "Not found" names what was asked for.
const char* failureToast(Kind kind, Media media, const Mailbox& box) {
  if (kind == Kind::Auto) return box.error;
  if (strcmp(box.user, kEvoTwitchNotFound) != 0) return box.user;
  return media == Media::Video ? kVideoGone : media == Media::Clip ? kClipGone : kChannelNotFound;
}

// A quality switch's lookup is back: step one of two. The old quality is
// still playing. If the switch may go ahead, the player is left here and the
// stream is stopped and restarted on the next pump call (pumpLeaving()): by
// then a frame without the video has been drawn and the player's GPU mode is
// off, as when the exit dialog stops a stream.
void onQualityResolved(const Mailbox& box) {
  evo::Application& app = evo::Application::getInstance();
  evo::IPlaybackController* pb = app.getPlaybackController();
  evo::ScreenManager* sm = app.getScreenManager();
  const bool ours = ownStreamPlaying(pb) && strcmp(g_playing.item, box.item) == 0;
  if (box.outcome != kOutStream) {
    // Said only while the stream it was for still plays and nothing else waits.
    if (box.outcome == kOutError && ours && !g_pending && !appQuitting())
      showError(failureToast(Kind::Quality, g_playing.media, box));
    startPending();
    return;
  }
  if (g_pending) {
    // A play request came in during the lookup: the user left this stream.
    evo_log("twitch: a play request came in, dropping the switch to %s", g_req_quality);
    startPending();
    return;
  }
  if (appQuitting()) {
    evo_log("twitch: the app is quitting, not switching %s", box.item);
    return;
  }
  if (!ours) {
    evo_log("twitch: %s stopped during the lookup (player state %d), not switching", box.item,
            pb ? static_cast<int>(pb->getPlaybackState()) : -1);
    return;
  }

  // The closest quality to the one asked for is the one already playing (the
  // row asked for is gone): a fresh list, nothing to restart.
  const int current = g_playing.quality_current;
  if (box.quality_chosen >= 0 && current >= 0 && current < g_playing.quality_count &&
      strcmp(box.qualities[box.quality_chosen].name, g_playing.qualities[current].name) == 0) {
    g_playing.quality_count = box.quality_count;
    memcpy(g_playing.qualities, box.qualities, sizeof g_playing.qualities);
    g_playing.quality_current = box.quality_chosen;
    evo_log("twitch: %s: %s is gone and %s plays already, not restarting", box.item, g_req_quality,
            g_playing.qualities[g_playing.quality_current].name);
    char note[160];
    snprintf(note, sizeof note, "%s isn't available now", g_switch_label);
    showToast(note);
    return;
  }

  // Only from the player or the quality picker: an exit dialog the user opened
  // meanwhile wins, and the toast says the switch was dropped.
  if (!sm) {
    evo_log_error("twitch: no screen manager");
    return;
  }
  const evo::ScreenId screen = sm->getCurrentScreenId();
  if (screen != evo::ScreenId::Player && screen != evo::ScreenId::AudioTrackPicker) {
    evo_log("twitch: %s: screen %d is up, not switching to %s", box.item, static_cast<int>(screen),
            g_req_quality);
    showToast("Quality not changed");
    return;
  }

  evo_log("twitch: %s: leaving the player to switch to %s", box.item, g_req_quality);
  sm->returnFromPlayback();
  memcpy(&g_switch_box, &box, sizeof box);
  g_phase = Phase::Leaving;
}

// Step two, on the pump call after the player was left: stop the old quality
// and start the new one, after checking again what step one checked.
void pumpLeaving() {
  g_phase = Phase::Idle;  // unless the start worker is launched below
  const Mailbox& box = g_switch_box;
  evo::IPlaybackController* pb = evo::Application::getInstance().getPlaybackController();
  const bool ours = ownStreamPlaying(pb) && strcmp(g_playing.item, box.item) == 0;
  // A video or clip carries on from here in the new quality. A video's
  // position is saved now: the restart keeps the pump from seeing it stop.
  double at = 0.0;
  if (ours && g_playing.media != Media::Live) at = pb->getPositionSeconds();
  if (ours && g_playing.media == Media::Video && g_watch.active && strcmp(g_watch.id, g_playing.item) == 0) {
    g_watch.position = at;
    if (g_watch.position != g_watch.saved && recordWatched(g_watch)) evo_twitch::progress_save();
  }
  // And stays paused if it was (or was about to be again: a switch that
  // comes before the last one's first picture).
  const bool wasPaused = ours && (pb->getPlaybackState() == evo::PlaybackState::Paused || g_playing.pause_after);
  // The player was left for this switch: whatever happens next, the old
  // quality doesn't play on behind another screen.
  if (ours) {
    evo_log("twitch: %s: stopping the stream to switch to %s", box.item, g_req_quality);
    pb->stopPlayback();
  }
  g_playing.quality_count = 0;  // nothing of ours plays until it has started
  if (g_pending) {
    evo_log("twitch: a play request came in, dropping the switch to %s", g_req_quality);
    startPending();
    return;
  }
  if (appQuitting()) {
    evo_log("twitch: the app is quitting, not switching %s", box.item);
    return;
  }
  if (!ours) {
    evo_log("twitch: %s stopped before the switch (player state %d), not starting it", box.item,
            pb ? static_cast<int>(pb->getPlaybackState()) : -1);
    return;
  }

  // The label of what the worker actually picked (the closest one when the
  // row asked for was gone).
  const char* label = box.quality_chosen >= 0 ? box.qualities[box.quality_chosen].label : g_switch_label;
  char still[160];
  snprintf(still, sizeof still, "Still switching to %s%s", label, kEllipsis);
  if (!launchStart(box, g_playing.media, g_playing.title, g_playing.name, g_playing.info, &g_playing.meta, at,
                   still))
    return;
  g_starting.pause_after = wasPaused;
  char note[160];
  snprintf(note, sizeof note, "Switching to %s%s", label, kEllipsis);
  showToast(note);
}

bool onTwitchHome() {
  evo::ScreenManager* sm = evo::Application::getInstance().getScreenManager();
  return sm && sm->getCurrentScreenId() == evo::ScreenId::TwitchHome;
}

// Drops the muted picture: silence and decode off, and its stream stopped.
// Main thread, never while the start worker is inside the controller (the
// callers check busy()).
void dropHomePreview() {
  g_home_preview = false;
  g_preview_full = false;
  previewSwitches(false);
  evo::IPlaybackController* pb = evo::Application::getInstance().getPlaybackController();
  if (pb && pb->getPlaybackState() != evo::PlaybackState::Stopped) pb->stopPlayback();
  g_playing.item[0] = '\0';
  g_playing.quality_count = 0;
}

// The picture of `item` didn't open, or died: the next try waits, longer
// each time in a row, and that channel waits longer still.
void previewFailed(const char* item) {
  const long long now = monoMs();
  const int doublings = std::min(g_preview_failures, 5);
  ++g_preview_failures;
  const long long wait = std::min(kPreviewRetryMs << doublings, kPreviewRetryMaxMs);
  g_preview_wait_until_ms = now + wait;
  copyUtf8(g_preview_failed, sizeof g_preview_failed, item ? item : "");
  g_preview_failed_until_ms = now + kPreviewSkipMs;
  evo_log("twitch: home preview: %s failed (%d in a row), the next try in %lld s", g_preview_failed,
          g_preview_failures, wait / 1000);
}

// Whether a picture of `login` may be started now (see previewFailed()).
bool previewMayTry(const char* login) {
  const long long now = monoMs();
  if (now < g_preview_wait_until_ms) return false;
  return now >= g_preview_failed_until_ms || strcmp(login, g_preview_failed) != 0;
}

void onPreviewResolved(const Mailbox& box) {
  const bool cancel = g_preview_cancel || g_pending;
  g_preview_cancel = false;
  if (cancel) {
    evo_log("twitch: home preview dropped");
    startPending();
    return;
  }
  if (box.outcome != kOutStream) {
    // Offline since the list was made, for subscribers only, refused, no
    // connection: each says why in the log, never in a toast.
    evo_log("twitch: home preview: %s would not open (%s)", g_req_id, box.outcome == kOutError ? box.error : "-");
    previewFailed(g_req_id);
    return;
  }
  if (appQuitting() || !onTwitchHome()) {
    evo_log("twitch: home preview: not on Home, not starting %s", box.item);
    return;
  }
  // Only into an empty player. What an error or a finished stream left there
  // is stopped here, on the main thread, as okToStart() does for a play
  // request; anything else means something else plays, and it stays.
  evo::IPlaybackController* pb = evo::Application::getInstance().getPlaybackController();
  if (!pb) return;
  const evo::PlaybackState state = pb->getPlaybackState();
  if (state == evo::PlaybackState::Error || state == evo::PlaybackState::Finished) {
    pb->stopPlayback();
  } else if (state != evo::PlaybackState::Stopped) {
    evo_log("twitch: home preview: the player is busy (state %d), not starting %s", static_cast<int>(state),
            box.item);
    return;
  }
  char still[160];
  snprintf(still, sizeof still, "Still starting %s%s", box.name, kEllipsis);
  if (!launchStart(box, Media::Live, box.title, box.name, box.info, nullptr, 0, still)) {
    previewFailed(box.item);
    return;
  }
  g_preview_full = box.quality_chosen >= 0 && box.quality_chosen == box.quality_wanted;
  evo_log("twitch: home preview: starting %s in %s", box.item,
          box.quality_chosen >= 0 ? box.qualities[box.quality_chosen].name : "?");
}

// Every frame, while the picture is up: it is dropped once it has died (an
// error, the end of the stream, a decoder that gave up, or no picture for
// kPreviewNoPictureMs), stopped here on the main thread, and the next try
// waits. A picture that shows clears the failures. Never while busy().
void previewTick() {
  if (!g_home_preview || evo_twitch_launcher_busy()) return;
  evo::IPlaybackController* pb = evo::Application::getInstance().getPlaybackController();
  char item[sizeof g_playing.item];
  memcpy(item, g_playing.item, sizeof item);
  const char* why = nullptr;
  if (!ownStreamPlaying(pb))
    why = "it stopped";
  else if (evo_pb_decode_fatal())
    why = "the decoder gave up";
  else if (evo_pb_is_eof())
    why = "the stream ended";
  else if (pb->isPictureReady()) {
    if (!g_preview_seen) {
      g_preview_seen = true;
      g_preview_failures = 0;
      evo_log("twitch: home preview: %s shows", item);
    }
    return;
  } else if (!g_preview_seen && monoMs() - g_preview_up_ms >= kPreviewNoPictureMs) {
    why = "no picture came";
  } else {
    return;
  }
  evo_log("twitch: home preview: %s dropped, %s (player state %d)", item, why,
          pb ? static_cast<int>(pb->getPlaybackState()) : -1);
  dropHomePreview();
  previewFailed(item);
}

void onResolved() {
  pthread_join(g_resolve_thread, nullptr);
  g_phase = Phase::Idle;  // unless a start worker is launched below
  const Kind kind = g_kind;

  Mailbox box;
  pthread_mutex_lock(&g_box_mx);
  memcpy(&box, &g_box, sizeof box);
  pthread_mutex_unlock(&g_box_mx);

  if (kind == Kind::Quality) {
    onQualityResolved(box);
    return;
  }
  if (kind == Kind::Preview) {
    onPreviewResolved(box);
    return;
  }

  const Media media = mediaOf(kind);
  if (box.outcome != kOutStream) {
    if (box.outcome == kOutError && !appQuitting()) showError(failureToast(kind, media, box));
    startPending();
    return;
  }
  if (g_pending) {  // cannot happen: a stood-down autoplay posts nothing
    evo_log_warn("twitch: dropping the play request for %s", g_pending_req.id);
    g_pending = false;
  }

  evo::Application& app = evo::Application::getInstance();
  evo::IPlaybackController* pb = app.getPlaybackController();
  evo::ScreenManager* sm = app.getScreenManager();
  if (!pb || !sm) {
    evo_log_error("twitch: no playback controller or screen manager");
    return;
  }
  if (appQuitting()) {
    evo_log("twitch: the app is quitting, not starting %s", box.item);
    return;
  }
  if (!okToStart(kind, box, pb, sm)) return;

  // A video picks up where it was left (the first 30 s and the last minute
  // start from the top). A clip starts from the top.
  const int resume = media == Media::Video ? evo_twitch::progress_resume_at(box.item) : 0;
  char still[160];
  snprintf(still, sizeof still, "Still starting %s%s", box.name, kEllipsis);
  if (!launchStart(box, media, box.title, box.name, box.info, media == Media::Live ? nullptr : &g_req_meta,
                   resume, still))
    return;
  g_starting.resumed = resume > 0;
  if (resume > 0)
    evo_log("twitch: starting playback of %s at %d s", box.item, resume);
  else
    evo_log("twitch: starting playback of %s", box.item);
  char note[160];
  snprintf(note, sizeof note, "Starting %s%s", box.name, kEllipsis);
  showToast(note);
}

void onStarted() {
  pthread_join(g_start_thread, nullptr);
  delete g_start_job;  // the controller keeps its own copy of the source
  g_start_job = nullptr;
  g_phase = Phase::Idle;

  evo::Application& app = evo::Application::getInstance();
  evo::IPlaybackController* pb = app.getPlaybackController();
  evo::ScreenManager* sm = app.getScreenManager();
  g_playing.quality_count = 0;
  if (g_kind == Kind::Preview) {
    // Left Home, quitting, or a play request came in while it started: the
    // picture goes, and the play request (if any) goes next.
    const bool wanted = !appQuitting() && !g_preview_cancel && !g_pending && onTwitchHome();
    if (g_start_ok && wanted) {
      evo_log("twitch: home preview: %s is up", g_starting.item);
      memcpy(&g_playing, &g_starting, sizeof g_playing);
      g_home_preview = true;
      g_preview_up_ms = monoMs();
      g_preview_seen = false;
    } else {
      if (g_start_ok) {
        evo_log("twitch: home preview: stopping %s", g_starting.item);
      } else {
        evo_log_error("twitch: home preview: %s didn't start", g_starting.item);
        previewFailed(g_starting.item);
      }
      // A failed start can leave Error behind: stopped too, so the next
      // start finds an empty player.
      dropHomePreview();
    }
    g_preview_cancel = false;
    startPending();
    evo_boot_log_flush();
    return;
  }
  if (g_start_ok) {
    if (appQuitting()) {
      // The held soft close goes ahead next frame; it must not find us playing.
      evo_log("twitch: the app is quitting, stopping the stream that just opened");
      if (pb) pb->stopPlayback();
    } else {
      evo_log("twitch: playback started");
      memcpy(&g_playing, &g_starting, sizeof g_playing);
      // A channel watched live goes first in Home's "Recently watched" (a
      // quality switch is the same watch going on).
      if (g_playing.media == Media::Live && g_kind != Kind::Quality && g_playing.info.login[0])
        evo_twitch::history_record_channel(g_playing.info.login, g_playing.info.name, g_playing.info.avatar_url);
      if (sm) sm->navigateTo(evo::ScreenId::Player);
      // Not paused yet: pauseTick() does it once the first picture is up.
      if (g_playing.pause_after)
        evo_log("twitch: it was paused before the switch, pausing it again once its picture is up");
      // Where it really starts: the controller's seek may have landed at
      // the top instead (then there is nothing to say).
      const double at = g_playing.resumed && pb ? pb->getPositionSeconds() : 0.0;
      if (at >= 1.0) {
        const std::string note = "Resumed at " + twitch::clockText(static_cast<int>(at));
        showToast(note.c_str());
      }
    }
  } else if (pb && pb->getPlaybackState() == evo::PlaybackState::Error) {
    // Every failure path in startPlaybackSource() that sets Error has already
    // shown its own toast ("Couldn't open the stream", ...) with the reason.
    evo_log_error("twitch: playback failed (the player said why)");
  } else {
    evo_log_error("twitch: playback failed");
    if (!appQuitting()) showError("Playback failed");
  }
  evo_boot_log_flush();
}

void pumpStarting() {
  if (softClosePastFirstStage()) {  // defensive fallback, see the top of the file
    int expected = kRunning;
    if (g_start_owner.compare_exchange_strong(expected, kAbandoned)) {
      // The worker now owns the result and the job; the main thread parks soon.
      pthread_detach(g_start_thread);
      g_start_job = nullptr;
      g_phase = Phase::Idle;
      evo_log("twitch: the app is closing while the stream opens; the start worker will stop it");
      return;
    }
    // Lost the race: the worker already reported, so onStarted() stops it here.
  }
  if (g_start_owner.load() == kRunning) {
    const long long now = monoMs();
    // Home's picture says nothing while it opens, unless the user asked to
    // quit: the soft close waits for it, and the screen shouldn't look dead.
    if ((g_kind != Kind::Preview || appQuitting()) && now - g_start_toast_ms >= 3000) {
      showToast(g_kind == Kind::Preview ? kPreviewClosing : g_start_still);
      g_start_toast_ms = now;
    }
    if (!g_start_slow_logged && now - g_start_t0_ms >= 45000) {
      evo_log_warn("twitch: start still running after 45 s");
      g_start_slow_logged = true;
    }
    return;
  }
  if (g_start_owner.load() == kReported) onStarted();
}

// "Already starting a stream", or a video or a clip: what is in flight.
const char* alreadyStarting() {
  const Kind k = g_pending ? g_pending_req.kind : g_kind;
  return k == Kind::Video ? "Already starting a video"
         : k == Kind::Clip ? "Already starting a clip"
                           : "Already starting a stream";
}

// A play request of any kind may be looked at at all: the self-test passed
// (toasted when not) and the app isn't quitting.
bool requestAllowed() {
  if (!selftestPassed()) {
    evo_log_error("twitch: play request refused because the self-test failed");
    showError(kSelftestFailed);
    return false;
  }
  if (appQuitting()) {
    evo_log("twitch: the app is quitting, ignoring a play request");
    return false;
  }
  return true;
}

// A play request of any kind (evo_twitch_play, _video, _clip), once
// requestAllowed(): the lookup, or a wait for the autoplay check or a quality
// switch to stand down (see the top of the file).
void request(const Request& req) {
  // Ours first: one lookup or start at a time. An autoplay check that has
  // not decided to play yet, or a quality switch before its start, does not
  // count; it stands down below.
  const bool autoDeciding = g_phase == Phase::Resolving && g_kind == Kind::Auto &&
                            g_auto_gate.load() == kGateDeciding;
  const bool qualityLookup = g_phase == Phase::Resolving && g_kind == Kind::Quality;
  const bool qualityLeaving = g_phase == Phase::Leaving;
  const bool previewLookup = g_phase == Phase::Resolving && g_kind == Kind::Preview;
  const bool previewStarting = g_phase == Phase::Starting && g_kind == Kind::Preview;
  if ((g_phase != Phase::Idle && !autoDeciding && !qualityLookup && !qualityLeaving && !previewLookup &&
       !previewStarting) ||
      g_pending || g_in_pump) {
    evo_log("twitch: already starting a stream, ignoring the play request for %s", req.id);
    showToast(alreadyStarting());
    return;
  }
  if (previewStarting) {
    // Home's picture is opening on the start worker, so the controller can't
    // be looked at now. The request waits; onStarted() stops the picture on
    // the main thread and lets it go.
    g_preview_cancel = true;
    g_pending_req = req;
    g_pending = true;
    evo_log("twitch: play request for %s, waiting for the home preview to stand down", g_pending_req.id);
    return;
  }
  if (qualityLeaving) {
    // The switch has left the player and stops its stream on the next pump
    // call; it drops itself there and this request goes next. Until then
    // the stream it is about to stop is all that plays.
    g_pending_req = req;
    g_pending = true;
    evo_log("twitch: play request for %s, the quality switch stands down", g_pending_req.id);
    return;
  }
  // Then EVO's other starters (see the top of the file).
  evo::Application& app = evo::Application::getInstance();
  evo::IPlaybackController* pb = app.getPlaybackController();
  evo::ScreenManager* sm = app.getScreenManager();
  if (!pb || !sm) {
    evo_log_error("twitch: no playback controller or screen manager");
    return;
  }
  if (const char* why = otherPlayback(pb, sm)) {
    evo_log("twitch: ignoring the play request for %s: screen %d, player state %d", req.id,
            static_cast<int>(sm->getCurrentScreenId()), static_cast<int>(pb->getPlaybackState()));
    showToast(why);
    return;
  }

  if (autoDeciding) {
    // The autoplay check stands down and this request goes as soon as its
    // worker has returned (onResolved()), unless the check just decided.
    int deciding = kGateDeciding;
    if (!g_auto_gate.compare_exchange_strong(deciding, kGateStoodDown)) {
      evo_log("twitch: autoplay starting, ignoring the play request for %s", req.id);
      showToast("Already starting a stream");
      return;
    }
    g_pending_req = req;
    g_pending = true;
    evo_log("twitch: play request for %s, the autoplay check stands down", g_pending_req.id);
    return;
  }
  if (previewLookup) {
    g_preview_cancel = true;
    g_pending_req = req;
    g_pending = true;
    evo_log("twitch: play request for %s, the home preview stands down", g_pending_req.id);
    return;
  }
  if (qualityLookup) {
    // The stream being switched has stopped (otherPlayback() passed), and
    // the switch is dropped once its lookup is back (onQualityResolved()).
    g_pending_req = req;
    g_pending = true;
    evo_log("twitch: play request for %s, the quality switch stands down", g_pending_req.id);
    return;
  }
  evo_log("twitch: play request for %s", req.id);
  beginRequest(req);
}

// A video id Twitch hands out: digits, and not too many.
bool validVideoId(const char* id) {
  const size_t n = strlen(id);
  if (n == 0 || n > 20) return false;
  for (size_t i = 0; i < n; ++i)
    if (id[i] < '0' || id[i] > '9') return false;
  return true;
}

// A clip slug: letters, digits, '-' and '_' ("AwkwardHelplessSalamanderSwiftRage-a1b2C3").
bool validClipSlug(const char* slug) {
  const size_t n = strlen(slug);
  if (n == 0 || n >= sizeof Request{}.id) return false;
  for (size_t i = 0; i < n; ++i) {
    const char c = slug[i];
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                    c == '_';
    if (!ok) return false;
  }
  return true;
}

void copyField(char* dst, size_t cap, const char* src) { copyUtf8(dst, cap, src ? src : ""); }

// The card's details, as the screen handed them over (info may be NULL).
void copyMeta(VideoMeta& m, const evo_twitch_video_info_t* info, int type) {
  memset(&m, 0, sizeof m);
  m.type = type;
  m.views = -1;
  if (!info) return;
  if (info->type >= EVO_TWITCH_VIDEO_ARCHIVE && info->type <= EVO_TWITCH_VIDEO_CLIP) m.type = info->type;
  copyField(m.title, sizeof m.title, info->title);
  copyField(m.login, sizeof m.login, info->channel_login);
  copyField(m.name, sizeof m.name, info->channel_name);
  copyField(m.game, sizeof m.game, info->category);
  copyField(m.age, sizeof m.age, info->age);
  copyField(m.published, sizeof m.published, info->published);
  copyField(m.curator, sizeof m.curator, info->curator);
  copyField(m.thumb, sizeof m.thumb, info->thumb_url);
  copyField(m.avatar, sizeof m.avatar, info->avatar_url);
  m.seconds = info->seconds > 0 ? info->seconds : 0;
  m.views = info->views;
}

// "archive", "highlight", "upload": a video's type as its progress keeps it.
const char* typeName(int type) {
  switch (type) {
    case EVO_TWITCH_VIDEO_HIGHLIGHT: return "highlight";
    case EVO_TWITCH_VIDEO_UPLOAD: return "upload";
    default: return "archive";
  }
}

// The position followed, into the video's saved progress, written out.
// The position followed, into the video's saved progress (written by the
// caller). False when there is nothing to measure it against: a broadcast
// still being recorded has no length yet.
bool recordWatched(Watching& w) {
  if (w.duration <= 0.0) return false;
  evo_twitch::VideoProgress e;
  e.id = w.id;
  e.seconds = static_cast<int>(w.position);
  e.duration = static_cast<int>(w.duration);
  e.title = w.meta.title;
  e.channel = w.meta.name[0] ? w.meta.name : w.meta.login;
  e.login = w.meta.login;
  e.thumb = w.meta.thumb;
  e.game = w.meta.game;
  e.published = w.meta.published;
  e.type = typeName(w.meta.type);
  evo_twitch::progress_record(e);
  w.saved = w.position;
  return true;
}

// Once per frame (see the top of the file): follows the video of ours that
// plays, and saves its position every kProgressSaveMs and once it is seen to
// have stopped. Never reads the controller while the start worker is in it.
void progressTick() {
  evo_twitch::progress_frame();
  if (evo_twitch_launcher_busy()) return;
  evo::IPlaybackController* pb = evo::Application::getInstance().getPlaybackController();
  const bool video = g_playing.media == Media::Video && ownStreamPlaying(pb);
  Watching& w = g_watch;
  if (w.active && (!video || strcmp(w.id, g_playing.item) != 0)) {
    // It stopped (or another one plays now): where it was last seen.
    if (w.position != w.saved && recordWatched(w)) evo_twitch::progress_save();
    if (w.duration > 0.0)
      evo_log("twitch: video %s: left at %d of %d s", w.id, static_cast<int>(w.position),
              static_cast<int>(w.duration));
    w.active = false;
  }
  if (!video) return;
  const long long now = monoMs();
  if (!w.active) {
    memset(&w, 0, sizeof w);
    w.active = true;
    copyUtf8(w.id, sizeof w.id, g_playing.item);
    w.meta = g_playing.meta;
    w.saved = -1.0;
    w.saved_ms = now;
  }
  // Not while a scrub is being dragged (the position is where it would land)
  // or a seek is under way.
  const evo::PlaybackState state = pb->getPlaybackState();
  if (state == evo::PlaybackState::Playing || state == evo::PlaybackState::Paused) {
    w.duration = pb->getDurationSeconds();
    w.position = pb->getPositionSeconds();
  }
  if (now - w.saved_ms >= kProgressSaveMs) {
    w.saved_ms = now;
    if (w.position != w.saved && recordWatched(w)) evo_twitch::progress_save();
  }
}

// Once per frame: a quality switch of a paused stream pauses the new one
// again, but only once its first picture is up. The decode thread decodes
// nothing while paused, so a pause straight after the start left the player
// black until the user pressed play. Anything the user does first wins (a
// pause of their own, a seek), and a stream that stops before then is
// forgotten. Never reads the controller while the start worker is in it.
void pauseTick() {
  if (!g_playing.pause_after || evo_twitch_launcher_busy()) return;
  evo::IPlaybackController* pb = evo::Application::getInstance().getPlaybackController();
  if (!ownStreamPlaying(pb)) {
    evo_log("twitch: %s stopped before its picture was up, not pausing it", g_playing.item);
    g_playing.pause_after = false;
    return;
  }
  if (pb->getPlaybackState() != evo::PlaybackState::Playing) {
    evo_log("twitch: %s was paused or moved by the user first, not pausing it", g_playing.item);
    g_playing.pause_after = false;
    return;
  }
  if (!pb->isPictureReady()) return;
  evo_log("twitch: %s: the picture is up, pausing it again", g_playing.item);
  g_playing.pause_after = false;
  pb->setPaused(true);
}

void pumpResolving() {
  if (g_note_ready.exchange(false)) {
    char note[sizeof g_box.note];
    pthread_mutex_lock(&g_box_mx);
    memcpy(note, g_box.note, sizeof note);
    pthread_mutex_unlock(&g_box_mx);
    if (!appQuitting() && g_kind != Kind::Preview) showToast(note);
  }
  if (g_resolve_done) onResolved();
}

}  // namespace

extern "C" void evo_twitch_launcher_start(void) noexcept {
  try {
    if (g_autoplay_tried) return;  // once per app launch
    g_autoplay_tried = true;

    if (!selftestPassed()) {
      evo_log_error("twitch: launcher off because the self-test failed");
      showError(kSelftestFailed);
      return;
    }
    if (g_phase != Phase::Idle) return;  // a play request got in first
    evo::ScreenManager* sm = evo::Application::getInstance().getScreenManager();
    g_auto_screen = sm ? sm->getCurrentScreenId() : evo::ScreenId::TwitchHome;
    beginPlay(Kind::Auto, nullptr);
  } catch (...) {
    evo_log_error("twitch: launcher start failed");
  }
}

extern "C" void evo_twitch_preview(const char* login) noexcept {
  try {
    if (!login || !login[0] || appQuitting()) return;
    if (g_phase != Phase::Idle || g_pending || evo_twitch_launcher_busy()) return;
    if (!onTwitchHome()) return;
    std::string normalized;
    try {
      normalized = twitch::normalizeLogin(login);
    } catch (const twitch::NotFoundError&) {
      return;
    }
    if (g_home_preview && strcmp(g_playing.item, normalized.c_str()) == 0) return;
    evo::IPlaybackController* pb = evo::Application::getInstance().getPlaybackController();
    if (!pb) return;
    if (g_home_preview) {
      // Another channel is on top now: the old picture goes, whether or not
      // the new one may start yet.
      evo_log("twitch: home preview: %s is no longer on top", g_playing.item);
      dropHomePreview();
    }
    if (!previewMayTry(normalized.c_str())) return;
    // Something of the user's own plays (or opens): it stays. What an error
    // or the end of a stream left is stopped once the lookup is back
    // (onPreviewResolved()).
    const evo::PlaybackState state = pb->getPlaybackState();
    if (state != evo::PlaybackState::Stopped && state != evo::PlaybackState::Error &&
        state != evo::PlaybackState::Finished)
      return;
    if (!selftestPassed()) return;
    g_preview_cancel = false;
    beginPlay(Kind::Preview, normalized.c_str());
    if (g_phase != Phase::Resolving) previewFailed(normalized.c_str());  // the worker didn't start
  } catch (...) {
    evo_log_error("twitch: home preview request failed");
  }
}

extern "C" void evo_twitch_preview_stop(void) noexcept {
  try {
    if (g_kind == Kind::Preview && (g_phase == Phase::Resolving || g_phase == Phase::Starting)) {
      // Still looking it up or opening it: onPreviewResolved() or onStarted()
      // drops it once the worker is back.
      g_preview_cancel = true;
      return;
    }
    if (evo_twitch_launcher_busy()) return;  // another start; the picture isn't up
    if (g_home_preview) {
      evo_log("twitch: home preview: stopping %s", g_playing.item);
      dropHomePreview();
    }
  } catch (...) {
    evo_log_error("twitch: home preview stop failed");
  }
}

extern "C" int evo_twitch_preview_active(void) noexcept { return g_home_preview ? 1 : 0; }

extern "C" int evo_twitch_preview_starting(void) noexcept {
  return g_kind == Kind::Preview && (g_phase == Phase::Resolving || g_phase == Phase::Starting) ? 1 : 0;
}

extern "C" int evo_twitch_preview_picture(const char* login) noexcept {
  try {
    if (!g_home_preview || !login || !login[0] || evo_twitch_launcher_busy()) return 0;
    if (strcmp(g_playing.item, login) != 0) {
      std::string normalized;
      try {
        normalized = twitch::normalizeLogin(login);
      } catch (...) {
        return 0;
      }
      if (normalized != g_playing.item) return 0;
    }
    evo::IPlaybackController* pb = evo::Application::getInstance().getPlaybackController();
    return pb && pb->isPictureReady() ? 1 : 0;
  } catch (...) {
    return 0;
  }
}

extern "C" int evo_twitch_preview_take(const char* login) noexcept {
  try {
    if (!g_home_preview || !login || !login[0] || evo_twitch_launcher_busy()) return 0;
    // Another play request is being looked up: it goes first (and the
    // caller's own request is turned down as usual).
    if (g_phase != Phase::Idle || g_pending) return 0;
    // A picture in a lower quality than Settings asks for is not taken over:
    // the caller plays the channel, which stops the picture first.
    if (!g_preview_full) return 0;
    std::string normalized;
    try {
      normalized = twitch::normalizeLogin(login);
    } catch (const twitch::NotFoundError&) {
      return 0;
    }
    if (strcmp(g_playing.item, normalized.c_str()) != 0) return 0;
    evo::Application& app = evo::Application::getInstance();
    evo::IPlaybackController* pb = app.getPlaybackController();
    evo::ScreenManager* sm = app.getScreenManager();
    if (!pb || !sm || !ownStreamPlaying(pb) || appQuitting()) return 0;
    g_home_preview = false;
    g_preview_full = false;
    previewSwitches(false);
    sm->navigateTo(evo::ScreenId::Player);
    if (g_playing.media == Media::Live && g_playing.info.login[0])
      evo_twitch::history_record_channel(g_playing.info.login, g_playing.info.name, g_playing.info.avatar_url);
    evo_log("twitch: home preview: opening %s in the player", g_playing.item);
    return 1;
  } catch (...) {
    evo_log_error("twitch: home preview open failed");
    return 0;
  }
}

extern "C" void evo_twitch_play(const char* login) noexcept {
  try {
    if (!login || !login[0]) {
      evo_log_warn("twitch: play request without a channel");
      return;
    }
    if (!requestAllowed()) return;
    std::string normalized;
    try {
      normalized = twitch::normalizeLogin(login);
    } catch (const twitch::NotFoundError&) {
      evo_log_warn("twitch: play request for an invalid channel name \"%.32s\"", login);
      showError(kChannelNotFound);
      return;
    }
    Request req;
    memset(&req, 0, sizeof req);
    req.kind = Kind::Play;
    copyUtf8(req.id, sizeof req.id, normalized);
    request(req);
  } catch (...) {
    evo_log_error("twitch: play request failed");
  }
}

extern "C" void evo_twitch_play_video(const char* id, const evo_twitch_video_info_t* info) noexcept {
  try {
    if (!id || !id[0]) {
      evo_log_warn("twitch: play request without a video");
      return;
    }
    if (!requestAllowed()) return;
    if (!validVideoId(id)) {
      evo_log_warn("twitch: play request for an invalid video id \"%.24s\"", id);
      showError(kVideoGone);
      return;
    }
    Request req;
    memset(&req, 0, sizeof req);
    req.kind = Kind::Video;
    copyUtf8(req.id, sizeof req.id, id);
    copyMeta(req.meta, info, EVO_TWITCH_VIDEO_ARCHIVE);
    request(req);
  } catch (...) {
    evo_log_error("twitch: video play request failed");
  }
}

extern "C" void evo_twitch_play_clip(const char* slug, const evo_twitch_video_info_t* info) noexcept {
  try {
    if (!slug || !slug[0]) {
      evo_log_warn("twitch: play request without a clip");
      return;
    }
    if (!requestAllowed()) return;
    if (!validClipSlug(slug)) {
      evo_log_warn("twitch: play request for an invalid clip slug \"%.64s\"", slug);
      showError(kClipGone);
      return;
    }
    Request req;
    memset(&req, 0, sizeof req);
    req.kind = Kind::Clip;
    copyUtf8(req.id, sizeof req.id, slug);
    copyMeta(req.meta, info, EVO_TWITCH_VIDEO_CLIP);
    req.meta.type = EVO_TWITCH_VIDEO_CLIP;
    request(req);
  } catch (...) {
    evo_log_error("twitch: clip play request failed");
  }
}

extern "C" void evo_twitch_launcher_pump(void) noexcept {
  if (g_in_pump) return;
  // Every frame, whatever the phase: the pause a quality switch carries
  // over, a video's position for its progress, Home's picture if it died,
  // and a history save that waited for the one before it.
  try {
    pauseTick();
    progressTick();
    previewTick();
    evo_twitch::history_frame();
  } catch (...) {
    evo_log_error("twitch: progress tick failed");
  }
  if (g_phase == Phase::Idle) return;
  g_in_pump = true;
  try {
    if (g_phase == Phase::Resolving)
      pumpResolving();
    else if (g_phase == Phase::Leaving)
      pumpLeaving();
    else
      pumpStarting();
  } catch (...) {
    evo_log_error("twitch: launcher pump failed");
  }
  g_in_pump = false;
}

extern "C" void evo_twitch_launcher_save_progress(void) noexcept {
  try {
    // The controller can't be read while the start worker is inside it.
    if (evo_twitch_launcher_busy()) return;
    progressTick();  // where the video is now, without waiting for the 15 s
    Watching& w = g_watch;
    if (w.active && w.position != w.saved && recordWatched(w))
      evo_log("twitch: video %s: saved at %d of %d s on the way out", w.id, static_cast<int>(w.position),
              static_cast<int>(w.duration));
    evo_twitch::progress_save_now();
    evo_twitch::history_save_now();
  } catch (...) {
    evo_log_error("twitch: saving the watch progress failed");
  }
}

bool evoTwitchLauncherIdle() noexcept { return g_phase == Phase::Idle && !g_pending; }

void evoTwitchPreviewForgetFailures() noexcept {
  g_preview_failures = 0;
  g_preview_wait_until_ms = 0;
  g_preview_failed_until_ms = 0;
}

extern "C" int evo_twitch_launcher_busy(void) noexcept {
  // g_phase is main-thread state, which is why this is main thread only.
  return g_phase == Phase::Starting && g_start_owner.load() == kRunning ? 1 : 0;
}

extern "C" int evo_twitch_launcher_starting(void) noexcept {
  if (g_pending) return 1;
  // Home's picture isn't one the user asked for: not a start, looked up or
  // opening.
  if (g_kind == Kind::Preview) return 0;
  if (g_phase == Phase::Starting || g_phase == Phase::Leaving) return 1;
  if (g_phase != Phase::Resolving) return 0;
  // The autoplay check is not a start until it has found the switch file, and
  // a quality switch's lookup isn't one either: the old quality still plays.
  if (g_kind == Kind::Quality) return 0;
  return g_kind == Kind::Play || g_kind == Kind::Video || g_kind == Kind::Clip || g_auto_gate.load() == kGateOn ? 1
                                                                                                                 : 0;
}

extern "C" int evo_twitch_qualities(const evo_twitch_quality_t** list) noexcept {
  if (list) *list = nullptr;
  try {
    // The controller can't be read while the start worker is inside it.
    if (evo_twitch_launcher_busy() || g_playing.quality_count <= 0) return 0;
    if (!ownStreamPlaying(evo::Application::getInstance().getPlaybackController())) return 0;
    if (list) *list = g_playing.qualities;
    return g_playing.quality_count;
  } catch (...) {
    return 0;
  }
}

extern "C" const evo_twitch_now_playing_t* evo_twitch_now_playing(void) noexcept {
  try {
    // The controller can't be read while the start worker is inside it.
    if (evo_twitch_launcher_busy()) return nullptr;
    if (!ownStreamPlaying(evo::Application::getInstance().getPlaybackController())) return nullptr;
    return &g_playing.info;
  } catch (...) {
    return nullptr;
  }
}

extern "C" int evo_twitch_current_quality(void) noexcept {
  if (evo_twitch_qualities(nullptr) <= 0) return -1;
  return g_playing.quality_current;
}

extern "C" int evo_twitch_switch_quality(const char* name) noexcept {
  try {
    if (!name || !name[0] || appQuitting()) return 0;
    if (g_phase != Phase::Idle || g_pending || g_in_pump) {
      evo_log("twitch: a lookup or start is in flight, ignoring the switch to %.31s", name);
      showToast(g_kind == Kind::Quality ? "Already changing the quality" : alreadyStarting());
      return 0;
    }
    const evo_twitch_quality_t* list = nullptr;
    const int count = evo_twitch_qualities(&list);
    if (count <= 0) {
      evo_log_warn("twitch: quality switch to %.31s with no stream of ours playing", name);
      return 0;
    }
    // A broadcast still being recorded has no length, so the player treats
    // it as live: a restart couldn't carry the position and would go back
    // to 0:00. Its quality stays.
    evo::IPlaybackController* pb = evo::Application::getInstance().getPlaybackController();
    if (g_playing.media == Media::Video && pb && pb->isLiveSource()) {
      evo_log("twitch: video %s is still being recorded, not switching to %.31s", g_playing.item, name);
      showToast(EVO_TWITCH_QUALITY_LOCKED);
      return 0;
    }
    int row = -1;
    for (int i = 0; i < count; ++i)
      if (strcmp(list[i].name, name) == 0) row = i;
    if (row < 0) {
      evo_log_warn("twitch: %s has no quality \"%.31s\"", g_playing.item, name);
      return 0;
    }
    if (row == g_playing.quality_current) return 0;

    copyUtf8(g_switch_label, sizeof g_switch_label, list[row].label);
    g_req_media = g_playing.media;
    beginResolve(Kind::Quality, g_playing.item, list[row].name);
    if (g_phase != Phase::Resolving) return 0;  // the worker didn't start (toasted)
    evo_log("twitch: quality switch for %s: %s -> %s", g_playing.item,
            g_playing.quality_current >= 0 ? list[g_playing.quality_current].name : "?", list[row].name);
    char note[160];
    snprintf(note, sizeof note, "Switching to %s%s", g_switch_label, kEllipsis);
    showToast(note);
    return 1;
  } catch (...) {
    evo_log_error("twitch: quality switch failed");
    return 0;
  }
}

extern "C" const char* evo_twitch_launcher_switching_channel(void) noexcept {
  // Only once the switch has stopped the old stream: before that, the old
  // stream is still what chat follows.
  if (g_phase != Phase::Starting || g_kind != Kind::Quality || g_starting.media != Media::Live) return nullptr;
  return g_starting.login[0] ? g_starting.login : nullptr;
}
