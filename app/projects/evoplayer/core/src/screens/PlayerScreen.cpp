#include "evo/screens/PlayerScreen.hpp"
#include "evo/Application.hpp"
#include "evo_rmlui_bridge.h"
#include "evo_playback.h"
#include "evo_demux.h"
#include "evo_audio_out.h"
#include "evo_vdec.h"
#include "evo_perf_monitor.h"
#include "evo_toast.h"
#include "evo_boot_log.h"
#include "evo_agc_runtime.h"
#include "evo_hw.h"
#include "pp_playback.h"   /* g_pp_pb.seek_discarding - the LOADING badge */
#include "evo_parallel_io.h"
/* Twitch chat (built with -Itwitch/include and the core's headers, see the
 * Makefile). TwitchScreenData.hpp brings the image service and evo_twitch.h. */
#include "evo/screens/TwitchScreenData.hpp"
#include "evo_twitch_auth.h"
#include "evo_twitch_chat.h"
#include "evo_twitch_chat_mode.h"
#include "evo_twitch_clip.h"
#include "evo_twitch_settings.h"
#include "twitch/qr.hpp"
#include "evo_keyboard.h"
#if defined(EVO_AGC_DEVICE)
#include "evo_hui.h"   /* evo_hui_set_osd_area */
#endif

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <sys/time.h>

extern evo_vdec *g_vdec;
extern AVFormatContext *play_fmt;
extern AVCodecContext *audio_ctx;
extern int perf_render_fps;
/* Bridge.cpp - reads the DEBUG OVERLAY setting for both FPS pills. */
int evo_fps_counter_enabled(void);
extern int perf_decode_fps;
extern int detected_audio_rate;
extern char current_media_path[768];
extern double resume_base_offset_seconds;
void draw_video_frame_to_fb(uint32_t *fb, int x, int y, int max_w, int max_h);
extern pp_playback g_pp_pb;
/* Bridge.cpp - raised at open and again whenever the demux queue runs dry on a
 * network source. Both decode threads are parked for as long as it is set. */
extern volatile int pb_prebuffer_hold;
}

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <deque>

namespace evo {

static uint64_t NowMs() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<uint64_t>(tv.tv_sec) * 1000ULL + static_cast<uint64_t>(tv.tv_usec / 1000ULL);
}

/*
 * The network reader is stuck waiting on the server and the video queue has
 * run dry. The demuxer is blocked inside that read, so the rebuffer hold never
 * arms and nothing else would say why the picture stopped: on hardware
 * 2026-10-07 a server outage showed a frozen frame, silently, for a minute.
 */
static bool readerStalled() {
    evo_pio_stats_t st;
    evo_pio_get_stats(&st);
    return st.active && st.stall_ms > 1500 && packet_queue_count(&video_packet_queue) < 3;
}

namespace {

/* The newest lines whose URLs are kept (m_chatUrls): as many as side mode can
 * hand the kit. */
constexpr std::size_t kChatLines = 40;
/* The newest lines handed to the kit, and so the pictures asked for: a few
 * more than fit (about 20 over the video, 22 beside it), never all 150. */
constexpr std::size_t kChatLinesOverlay = 25;
constexpr std::size_t kChatLinesSide = 40;

/* Picture heights asked of the image service: an emote at scale 2 is 56 px,
 * badges come at 36. */
constexpr int kChatEmoteScale = 2;
constexpr int kChatEmoteH = 56;
constexpr int kChatBadgeH = 36;

/* An R1 press is written to the settings file once the presses stop. */
constexpr uint64_t kChatSaveDelayMs = 1500;

/* How often the player asks for the channel playing again, for the OSD's
 * viewer count (and a title or category that changed meanwhile). */
constexpr unsigned long long kNowPlayingRefreshMs = 60000;

/* Frames the clip card is still drawn after it closes, while the kit fades
 * it out (about half a second). */
constexpr int kClipFadeFrames = 30;
/* A toast's lower edge while a video's timeline is up (its track is at 920
 * on the OSD's 1080p canvas). */
constexpr int kToastOverTimeline = 892;

/* The stats panel (Options) is for dev builds, the ones packaged with
 * --usb-remote. A release build ignores Options in the player and shows no
 * hint for it. */
#if defined(EVO_USB_REMOTE)
constexpr bool kStatsPanel = true;
#else
constexpr bool kStatsPanel = false;
#endif

/* Twitch's limit for one chat message. */
constexpr int kChatMessageMax = 500;
static_assert(kChatMessageMax <= EVO_KEYBOARD_MAX_LEN, "the keyboard must take a whole chat message");

const char* chatModeName(twitch::ChatMode mode) {
    switch (mode) {
        case twitch::ChatMode::Side:    return "Next to the video";
        case twitch::ChatMode::Overlay: return "Over the video";
        default:                        return "Off";
    }
}

int osdChatMode(twitch::ChatMode mode) {
    switch (mode) {
        case twitch::ChatMode::Side:    return EVO_OSD_CHAT_SIDE;
        case twitch::ChatMode::Overlay: return EVO_OSD_CHAT_OVERLAY;
        default:                        return EVO_OSD_CHAT_OFF;
    }
}

/* The channel's display name from the player's title: the launcher titles a
 * Twitch stream "<name> · <stream title>", or just "<name>" when there is no
 * title to show (evo_twitch_launcher.cpp, playerTitle). The login when the
 * title is empty. */
std::string channelName(const std::string& title, const std::string& login) {
    if (title.empty()) return login;
    return title.substr(0, title.find(" \xC2\xB7 "));
}

/* EVO's own keyboard is up. The system IME draws itself, outside our frame. */
bool virtualKeyboardUp() {
    return evo_keyboard_is_open() && !evo_keyboard_is_native_active();
}

/* A Twitch stream is playing (live: it has chat). Never asked while the
 * launcher's worker is starting one: the controller has no locking
 * (evo_twitch.h). */
bool twitchPlaying(IPlaybackController* playback) {
    if (!playback || evo_twitch_launcher_busy()) return false;
    const PlaybackSource* src = playback->currentSource();
    return src && evo_twitch::is_twitch_source(src->provider, src->item_id);
}

/* A Twitch past broadcast, highlight, upload or clip is playing: a timeline
 * and no chat. The same rule. */
bool twitchVideoPlaying(IPlaybackController* playback) {
    if (!playback || evo_twitch_launcher_busy()) return false;
    const PlaybackSource* src = playback->currentSource();
    return src && evo_twitch::is_twitch_video_source(src->provider, src->item_id);
}

/* A count the short way, as the kit writes viewers: 999, 1.2K, 12K, 1.1M. */
std::string shortCount(int n) {
    char b[32];
    if (n < 1000)
        std::snprintf(b, sizeof b, "%d", n < 0 ? 0 : n);
    else if (n < 10000)
        std::snprintf(b, sizeof b, "%.1fK", n / 1000.0);
    else if (n < 1000000)
        std::snprintf(b, sizeof b, "%dK", n / 1000);
    else
        std::snprintf(b, sizeof b, "%.1fM", n / 1000000.0);
    std::string out = b;
    const std::size_t dot0 = out.find(".0");
    if (dot0 != std::string::npos) out.erase(dot0, 2);  /* "2.0K" reads "2K" */
    return out;
}

/* The line under a video's title on the OSD: "Past broadcast · 3 days ago",
 * "Highlight · ...", "Upload · ...", or "Clip by <name> · 12K views". */
std::string videoMeta(const evo_twitch_now_playing_t& np) {
    const char* dot = " \xC2\xB7 ";
    std::string out;
    if (np.video_type == EVO_TWITCH_VIDEO_CLIP) {
        out = np.curator[0] ? std::string("Clip by ") + np.curator : std::string("Clip");
        if (np.views >= 0) out += dot + shortCount(np.views) + (np.views == 1 ? " view" : " views");
        return out;
    }
    out = np.video_type == EVO_TWITCH_VIDEO_HIGHLIGHT ? "Highlight"
          : np.video_type == EVO_TWITCH_VIDEO_UPLOAD  ? "Upload"
                                                      : "Past broadcast";
    if (np.age[0]) out += dot + std::string(np.age);
    return out;
}

/* "#RRGGBB" as 0xRRGGBB; white for anything else (the app's own lines have
 * no colour). */
unsigned int chatColor(const std::string& hex) {
    if (hex.size() != 7 || hex[0] != '#') return 0xFFFFFF;
    unsigned int v = 0;
    for (std::size_t i = 1; i < 7; ++i) {
        const char c = hex[i];
        unsigned int d;
        if (c >= '0' && c <= '9')      d = static_cast<unsigned int>(c - '0');
        else if (c >= 'a' && c <= 'f') d = static_cast<unsigned int>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = static_cast<unsigned int>(c - 'A' + 10);
        else return 0xFFFFFF;
        v = (v << 4) | d;
    }
    return v;
}

int chatKind(const twitch::ChatLine& line) {
    if (line.status) return EVO_HUI_TWITCH_CHAT_STATUS;
    if (line.notice) return EVO_HUI_TWITCH_CHAT_NOTICE;
    if (line.action) return EVO_HUI_TWITCH_CHAT_ACTION;
    return EVO_HUI_TWITCH_CHAT_MESSAGE;
}

} // namespace

PlayerScreen::PlayerScreen()
    : StatefulScreen("PlayerScreen")
    , m_playerFsm(PlayerScreenState::NormalPlayback, "PlayerPresenterFSM")
{
    initPlayerStateMachine();
}

void PlayerScreen::initPlayerStateMachine() {
    m_playerFsm
        .addState(PlayerScreenState::NormalPlayback, "NormalPlayback")
        .addState(PlayerScreenState::OsdVisible, "OsdVisible")
        .addState(PlayerScreenState::Scrubbing, "Scrubbing")
        .addState(PlayerScreenState::StatsOverlay, "StatsOverlay");

    m_playerFsm
        .addTransition(PlayerScreenState::NormalPlayback, PlayerScreenEvent::ShowOsd, PlayerScreenState::OsdVisible)
        .addTransition(PlayerScreenState::NormalPlayback, PlayerScreenEvent::StartScrub, PlayerScreenState::Scrubbing)
        .addTransition(PlayerScreenState::NormalPlayback, PlayerScreenEvent::ToggleStats, PlayerScreenState::StatsOverlay)
        .addTransition(PlayerScreenState::OsdVisible, PlayerScreenEvent::HideOsd, PlayerScreenState::NormalPlayback)
        .addTransition(PlayerScreenState::OsdVisible, PlayerScreenEvent::StartScrub, PlayerScreenState::Scrubbing)
        .addTransition(PlayerScreenState::OsdVisible, PlayerScreenEvent::ToggleStats, PlayerScreenState::StatsOverlay)
        .addTransition(PlayerScreenState::Scrubbing, PlayerScreenEvent::EndScrub, PlayerScreenState::OsdVisible)
        .addTransition(PlayerScreenState::StatsOverlay, PlayerScreenEvent::ToggleStats, PlayerScreenState::NormalPlayback)
        .addTransition(PlayerScreenState::StatsOverlay, PlayerScreenEvent::ShowOsd, PlayerScreenState::OsdVisible);
}

void PlayerScreen::toggleStatsForNerds() {
    if (!kStatsPanel) return;
    m_showStatsForNerds = !m_showStatsForNerds;
    m_playerFsm.postEvent(PlayerScreenEvent::ToggleStats);
}

bool PlayerScreen::hasActiveOverlay() const {
    if (m_osdVisibilityAlpha > 0 || m_showStatsForNerds) {
        return true;
    }
    /* The clip card, while it shows or fades out: like the OSD, every frame
     * it is drawn in is marked for a clear. */
    if (m_clipCardUntilMs != 0 || m_clipCardFadeFrames > 0) {
        return true;
    }
    /* EVO's keyboard over the video (Triangle, to write to chat). Every frame
     * it is drawn is then marked for a clear, so its dimmed backdrop can't
     * stack up on a buffer that skips the clear. */
    return virtualKeyboardUp();
}

void PlayerScreen::onEnter() {
    StatefulScreen::onEnter();
    m_osdVisibilityAlpha = 255;
    m_controlsLastUsedMs = NowMs();
    m_playerFsm.postEvent(PlayerScreenEvent::ShowOsd);
}

void PlayerScreen::onExit() {
    /* A picker, the exit dialog or another screen: an R1 change waiting to
     * be written goes now. */
    flushChatSave();
    /* The clip card stays with the stream it was made on: it doesn't come
     * back with the player. */
    m_clipCardUntilMs = 0;
    m_clipCardFadeFrames = 0;
    toast_set_bottom(0);  /* the pages' place again */
    StatefulScreen::onExit();
}

void PlayerScreen::flushChatSave() {
    if (!m_chatSavePending) return;
    m_chatSavePending = false;
    evo_twitch::save_settings(evo_twitch::settings());
}

bool PlayerScreen::handleInput(uint32_t pressed, uint32_t held, uint32_t released) {
    (void)released;
    auto playback = Application::getInstance().getPlaybackController();
    auto screenMgr = Application::getInstance().getScreenManager();
    if (!playback) return false;

    /* A release build: Options does nothing here, not even bring up the
     * controls (kStatsPanel). */
    if (!kStatsPanel) {
        pressed &= ~PadButtons::Options;
    }

    /* Circle closes the clip card first; the exit dialog comes once it's
     * gone. */
    if ((pressed & PadButtons::Circle) && m_clipCardUntilMs != 0) {
        closeClipCard();
        return true;
    }

    /* Square makes a clip of a live Twitch stream (Square has nothing else
     * to do in the player). Before the controls' timer, like R1 and
     * Triangle: the OSD isn't needed for it. */
    if ((pressed & PadButtons::Square) && twitchPlaying(playback)) {
        makeClip(playback);
        return true;
    }

    /* R1 changes the chat while a Twitch stream plays (live: R1 has nothing
     * else to do there): beside the video, over it, off. Before the controls'
     * timer, so it doesn't bring the OSD up. The change holds at once; the
     * file is written once the presses stop (update) or the player is left
     * (onExit). */
    if ((pressed & PadButtons::R1) && twitchPlaying(playback)) {
        const twitch::ChatMode mode = twitch::nextChatMode(evo_twitch::settings().chatMode);
        evo_twitch::set_chat_mode(mode);
        m_chatSavePending = true;
        m_chatSaveDueMs = NowMs() + kChatSaveDelayMs;
        toast("Chat", chatModeName(mode));
        return true;
    }

    /* Triangle writes to the chat of a Twitch stream, whose aspect stays the
     * Settings default. Before the controls' timer too: the OSD isn't needed
     * for typing. Any other source keeps the aspect below. */
    if ((pressed & PadButtons::Triangle) && twitchPlaying(playback)) {
        openChatKeyboard(playback);
        return true;
    }

    if (pressed) {
        m_controlsLastUsedMs = NowMs();
    }

    // Scrub hold update
    playback->updateScrubHold(held);

    /* A Twitch video or clip: no chat (R1, Triangle) and no chapters (L1),
     * and its aspect stays the Settings default like a stream's. Left/Right
     * seek below, Cross pauses. */
    const bool twitchVideo = twitchVideoPlaying(playback);
    if (twitchVideo && (pressed & (PadButtons::L1 | PadButtons::R1 | PadButtons::Triangle)))
        return true;

    if (pressed & PadButtons::Cross) {
        uint64_t now = NowMs();
        if (now - m_lastPlayPauseToggleMs >= 150) {
            m_lastPlayPauseToggleMs = now;
            // Toggle play / pause. No toast: the controls come up with it
            // and stay while it is paused, with the pause sign.
            playback->togglePause();
        }
        return true;
    }
    if (pressed & PadButtons::Circle) {
        // Stop playback confirmation prompt
        if (screenMgr) {
            screenMgr->navigateTo(ScreenId::ExitConfirm);
        }
        return true;
    }
    if (pressed & PadButtons::Options) {
        toggleStatsForNerds();  /* the panel itself says so */
        return true;
    }
    if (pressed & PadButtons::Triangle) {
        /* Aspect ratio, as it has always been on this screen. */
        playback->cycleViewMode();
        toast("Aspect", playback->getViewMode() == ViewMode::Fit ? "Fit" :
                        playback->getViewMode() == ViewMode::Fill ? "Fill" : "Stretch");
        return true;
    }
    /* Live TV has no timeline: seek and chapter keys are swallowed, not sent
     * to a demuxer that has nothing behind the live edge. */
    if (playback->isLiveSource() &&
        (pressed & (PadButtons::L1 | PadButtons::R1 | PadButtons::Left | PadButtons::Right)))
        return true;
    if (pressed & PadButtons::L1) {
        playback->jumpChapter(-1);
        return true;
    }
    if (pressed & PadButtons::R1) {
        playback->jumpChapter(1);
        return true;
    }
    if (pressed & PadButtons::Left) {
        playback->moveScrub(-10.0);
        return true;
    }
    if (pressed & PadButtons::Right) {
        playback->moveScrub(10.0);
        return true;
    }
    if (pressed & PadButtons::R2) {
        /* The quality picker, for a Twitch stream, video or clip (the only
         * kinds with a list of qualities: evo_twitch_qualities()). */
        if (!playback->isScrubbing() && screenMgr && (twitchVideo || twitchPlaying(playback))) {
            const PlaybackSource* src = playback->currentSource();
            const char* what = !twitchVideo ? "stream" : src && src->provider == "twitch-clip" ? "clip" : "video";
            char note[64];
            const int qualities = evo_twitch_qualities(nullptr);
            if (twitchVideo && playback->isLiveSource()) {
                /* A broadcast still being recorded: no length, so a restart
                 * couldn't keep its place (evo_twitch.h). */
                toast(EVO_TWITCH_QUALITY_LOCKED, nullptr);
            } else if (qualities > 1) {
                screenMgr->navigateTo(ScreenId::AudioTrackPicker);
            } else if (qualities == 1) {
                std::snprintf(note, sizeof note, "This %s has no other qualities", what);
                toast(note, nullptr);
            } else {  /* finished, or not one the launcher started */
                std::snprintf(note, sizeof note, "The %s isn't playing", what);
                toast(note, nullptr);
            }
        }
        return true;
    }

    return false;
}

void PlayerScreen::openChatKeyboard(IPlaybackController* playback) {
    const evo_twitch::AuthView& auth = evo_twitch::auth_view();
    if (auth.state == evo_twitch::AuthState::Unknown) {
        /* auth.json isn't read yet: a moment after start, or until the
         * sandbox opens. */
        toast("Checking your Twitch login", nullptr);
        return;
    }
    if (auth.state != evo_twitch::AuthState::LoggedIn) {
        toast("Log in under Settings to chat", nullptr);
        return;
    }
    const PlaybackSource* src = playback->currentSource();
    if (!src) return;
    m_chatSendChannel = src->item_id;
    char title[128];
    std::snprintf(title, sizeof title, "Send a message to %s",
                  channelName(src->title, src->item_id).c_str());
    /* A message to this channel that didn't go out in the last 2 minutes
     * comes back for another try instead of being typed again. Peeked, not
     * taken: cancelling the keyboard must not lose it. */
    std::string draft;
    evo_twitch::chat_peek_draft(src->item_id, &draft);
    evo_keyboard_open_ex(title, draft.c_str(), kChatMessageMax, EVO_KEYBOARD_ENTER_SEND,
                         &PlayerScreen::OnChatSubmitted, this);
}

void PlayerScreen::OnChatSubmitted(const char* text, void* userdata) noexcept {
    try {
        PlayerScreen* self = static_cast<PlayerScreen*>(userdata);
        if (!self || !text) return;
        /* Only taken now, so it's gone once the user has had another try at
         * it; a fresh failure below keeps a new one on its own. */
        std::string old;
        evo_twitch::chat_take_draft(self->m_chatSendChannel, &old);
        /* Queued for the send worker; a failure comes back later as a
         * notice, which Application toasts wherever the user is by then. */
        const char* refused = evo_twitch::chat_send(self->m_chatSendChannel, text);
        if (refused) toast_error("Chat", refused);  /* "Message not sent: ..." */
    } catch (...) {
        toast_error("Chat", "Message not sent: something went wrong");
    }
}

void PlayerScreen::makeClip(IPlaybackController* playback) {
    const PlaybackSource* src = playback->currentSource();
    if (!src) return;
    const char* refused = evo_twitch::clip_make(src->item_id);
    /* A refusal ("Clips need a login") says what to do; nothing broke. */
    toast(refused ? refused : "Making a clip\xE2\x80\xA6", nullptr);
}

void PlayerScreen::takeClipResult() {
    try {
        evo_twitch::ClipResult r;
        if (!evo_twitch::clip_take_result(&r)) return;
        if (!r.made) {
            toast_error("Couldn't make the clip", r.error.c_str());
            return;
        }
        twitch::QrMatrix qr = twitch::makeQr(std::string(evo_twitch::kClipPageUrl) + r.id);
        bool card = qr.size > 0;
#if defined(EVO_AGC_DEVICE)
        card = card && !evo_rmlui_twitch_fallback_active();
#else
        card = false;
#endif
        if (!card) {
            /* Nothing to draw the card with: the address, to type in. */
            const std::string page = std::string(evo_twitch::kClipPageShort) + r.id;
            toast_success("Clip made", page.c_str());
            return;
        }
        m_clipQr.swap(qr.dark);
        m_clipQrSize = qr.size;
        m_clipCardUntilMs = NowMs() + evo_twitch::kClipCardMs;
        m_clipCardFadeFrames = 0;
    } catch (...) {
        toast_error("Couldn't make the clip", "Something went wrong");
    }
}

void PlayerScreen::closeClipCard() {
    if (m_clipCardUntilMs == 0) return;
    m_clipCardUntilMs = 0;
    m_clipCardFadeFrames = kClipFadeFrames;
}

void PlayerScreen::renderClipCard(int chatMode, int width, int height) noexcept {
    const bool up = m_clipCardUntilMs != 0;
#if defined(EVO_AGC_DEVICE)
    evo_hui_twitch_clip_params_t p;
    std::memset(&p, 0, sizeof(p));
    p.visible = up ? 1 : 0;
    p.qr = m_clipQr.empty() ? nullptr : m_clipQr.data();
    p.qr_size = m_clipQr.empty() ? 0 : m_clipQrSize;
    if (up) {
        const uint64_t now = NowMs();
        const uint64_t left = now < m_clipCardUntilMs ? m_clipCardUntilMs - now : 0;
        p.time_left = static_cast<float>(left) / static_cast<float>(evo_twitch::kClipCardMs);
    }
    /* The OSD's area: the whole screen, or the video's box beside chat. */
    float ax, ay, aw, ah, scale, dx, dy;
    evo_twitch::chat_video_box(static_cast<twitch::ChatMode>(chatMode), &ax, &ay, &aw, &ah);
    evo_hui_osd_area_fit(ax, ay, aw, ah, &scale, &dx, &dy);
    p.area_x = dx;
    p.area_y = dy;
    p.area_w = EVO_CHAT_CANVAS_W * scale;
    p.area_h = EVO_CHAT_CANVAS_H * scale;
    evo_hui_twitch_clip_update(&p);
    evo_rmlui_render_kit_screen(evo_hui_render_twitch_clip, width, height);
#else
    (void)chatMode;
    (void)width;
    (void)height;
#endif
    /* Faded out: both buffers get one clear, so nothing of it stays over a
     * frame that skips the clear. */
    if (!up && m_clipCardFadeFrames > 0 && --m_clipCardFadeFrames == 0) {
        m_chatClearFrames = 2;
        m_clipQr.clear();
        m_clipQrSize = 0;
    }
}

void PlayerScreen::update(double deltaMs) {
    StatefulScreen::update(deltaMs);
    m_playerFsm.update(deltaMs);

    if (m_chatSavePending && NowMs() >= m_chatSaveDueMs) flushChatSave();

    /* A clip made with Square: its card, or why there is none. The card goes
     * by itself after its time. */
    takeClipResult();
    if (m_clipCardUntilMs != 0 && NowMs() >= m_clipCardUntilMs) closeClipCard();

    auto playback = Application::getInstance().getPlaybackController();
    if (!playback) return;

    refreshNowPlaying(playback);

    playback->tickScrubAutoCommit();

    /*
     * The decoder gave up (repeated failures, or a hardware call that stopped
     * returning - #39). The engine has already raised its error toast; without
     * this the player sat on a frozen picture with the controls up and nothing
     * to say why. Leave through the same path as STOP so the toast lands on the
     * screen the user returns to.
     */
    if (evo_pb_decode_fatal()) {
        playback->stopPlayback();
        if (auto sm = Application::getInstance().getScreenManager())
            sm->returnFromPlayback();
        return;
    }

    uint64_t now = NowMs();
    /*
     * Hold the OSD up once the clip has finished.
     *
     * Nothing in the list below is true at EOF - the clock simply stops, it is
     * not "paused", and the 3.5s input timer has long expired - so the OSD
     * faded out and the screen was left showing a held last frame with no
     * indication of what had happened or what the controls were. evo_pb_is_eof()
     * stays set for as long as the finished file is open, so the OSD stays up
     * until something actually changes (a seek back clears it).
     */
    const bool ended = (evo_pb_is_eof() != 0);

    /*
     * Ask the playback engine whether it is paused, not the state machine.
     *
     * Those two disagree. A D-pad seek sets `player_paused` directly
     * (evo_demux.c), which doesn't go through the FSM - so isPaused()
     * reported false while the picture was genuinely stopped, the 3.5s input
     * timer ran out, and the OSD faded away over a paused frame with nothing
     * on screen to say why.
     *
     * The PAUSED badge below deliberately keeps using isPaused(): the engine
     * flag is also raised for the moment a seek is in flight, and a badge that
     * blinked on every seek would be worse than one that is occasionally late.
     */
    const bool reallyPaused = (evo_pb_is_paused() != 0) || playback->isPaused();

    /* Hold the OSD active while video frames are still buffering into the
     * decoder - and while a seek is: on a network stream that is seconds of a
     * still picture that would otherwise read as frozen. */
    /* Say so once when the server stops the picture, and once when it is back.
     * Raised here rather than in the reader, which stalls long before anyone
     * can see it: the read-ahead keeps playing for half a minute. */
    const bool stalled = readerStalled();
    if (stalled && !m_stallToastShown) {
        m_stallToastShown = true;
        toast_error("Connection lost", "Trying again\xE2\x80\xA6");
    } else if (!stalled && m_stallToastShown && !pb_prebuffer_hold) {
        m_stallToastShown = false;
        toast_success("Back online", "The stream is playing again");
    }

    const bool isBuffering = !playback->isMusicMode() &&
                             (!video_frame_loaded || g_pp_pb.seek_discarding ||
                              pb_prebuffer_hold || stalled);
    if (isBuffering) {
        m_controlsLastUsedMs = now;
    }

    bool wantsControls = playback->isMusicMode() || ended ||
                         ((now - m_controlsLastUsedMs < 3500) || reallyPaused || m_showStatsForNerds) ||
                         playback->isScrubbing() || isBuffering;

    if (playback->isScrubbing()) {
        m_playerFsm.postEvent(PlayerScreenEvent::StartScrub);
    } else if (wantsControls) {
        m_playerFsm.postEvent(PlayerScreenEvent::ShowOsd);
    } else {
        m_playerFsm.postEvent(PlayerScreenEvent::HideOsd);
    }

    // Smooth OSD fade in/out
    if (wantsControls) {
        if (m_osdVisibilityAlpha < 255) {
            m_osdVisibilityAlpha = std::min(255, m_osdVisibilityAlpha + 42);
        }
    } else {
        if (m_osdVisibilityAlpha > 0) {
            m_osdVisibilityAlpha = std::max(0, m_osdVisibilityAlpha - 28);
        }
    }
}

void PlayerScreen::refreshNowPlaying(IPlaybackController* playback) {
    const bool live = twitchPlaying(playback);
    if (!live && !twitchVideoPlaying(playback)) return;
    const evo_twitch_now_playing_t* np = evo_twitch_now_playing();
    if (!np) return;
    const unsigned long long now = evo_twitch::data_now_ms();
    constexpr unsigned long long kNever = ~0ull;
    if (np->stamp != m_nowPlayingStamp) {
        /* Another stream. The launcher has just looked its channel up; the
         * autoplay may not have had everything, so that one asks now. A
         * video or clip asks once, for its channel's picture, when its card
         * didn't have it (and knew the channel). */
        m_nowPlayingStamp = np->stamp;
        if (live) {
            const bool complete = np->viewers >= 0 && np->avatar_url[0] != '\0';
            m_nowPlayingDueMs = complete ? now + kNowPlayingRefreshMs : now;
        } else {
            m_nowPlayingDueMs = np->login[0] != '\0' && np->avatar_url[0] == '\0' ? now : kNever;
        }
    }
    if (now < m_nowPlayingDueMs) return;
    m_nowPlayingDueMs = live ? now + kNowPlayingRefreshMs : kNever;
    evo_twitch::request_now_playing(np->login);
}

void PlayerScreen::fillTwitchOsd(evo_playback_osd_params_t& p, const PlaybackSource* src, int chatMode,
                                 bool video) {
    p.twitch = 1;
    p.twitch_video = video ? 1 : 0;
    p.chat_mode = video ? EVO_OSD_CHAT_OFF : chatMode;
    p.can_chat = !video && evo_twitch::auth_view().state == evo_twitch::AuthState::LoggedIn ? 1 : 0;
    /* Logged in: Square makes a clip. A login from before clips gets the
     * hint too; pressing it says to log in again. */
    p.can_clip = p.can_chat;

    /* The quality playing, without the "(source)" the picker adds. */
    m_osdQuality[0] = '\0';
    const evo_twitch_quality_t* qualities = nullptr;
    const int count = evo_twitch_qualities(&qualities);
    const int current = evo_twitch_current_quality();
    if (current >= 0 && current < count) {
        std::snprintf(m_osdQuality, sizeof m_osdQuality, "%s", qualities[current].label);
        char* suffix = std::strstr(m_osdQuality, " (source)");
        if (suffix) *suffix = '\0';
    }
    p.quality = m_osdQuality;

    /* The channel: the launcher's details, or the data service's when they
     * are newer (it asks once a minute, refreshNowPlaying). The strings stay
     * where they are until this frame's render has returned. */
    const evo_twitch_now_playing_t* np = evo_twitch_now_playing();
    if (!np) {
        p.channel = p.channel_login = src ? src->item_id.c_str() : "";
        return;
    }
    p.channel = np->name;
    p.channel_login = np->login;
    p.stream_title = np->title;
    p.category = np->game;
    p.viewers = np->viewers;
    if (video) {
        if (np->stamp != m_osdVideoMetaStamp) {
            m_osdVideoMetaStamp = np->stamp;
            m_osdVideoMeta = videoMeta(*np);
        }
        p.video_meta = m_osdVideoMeta.c_str();
    }
    const char* avatar = np->avatar_url;
    const evo_twitch::Result<evo_twitch::ChannelPage>& fresh = evo_twitch::now_playing();
    if (fresh.status == evo_twitch::Status::Ready && fresh.stamp > np->stamp && np->login[0] &&
        fresh.value.login == np->login) {
        /* A live stream takes everything newer; a video only its channel's
         * name and picture (the stream on now is another thing). */
        if (fresh.value.live && !video) {
            p.stream_title = fresh.value.title.c_str();
            p.category = fresh.value.game.c_str();
            p.viewers = fresh.value.viewers;
        }
        if (fresh.value.live || video) p.channel = fresh.value.displayName.c_str();
        if (!fresh.value.avatarUrl.empty() && (fresh.value.live || video)) avatar = fresh.value.avatarUrl.c_str();
    }
    if (avatar[0]) {
        const evo_hui_twitch_image_t pic =
            twitch_screen::picture(avatar, twitch_screen::kAvatarW, twitch_screen::kAvatarH);
        p.avatar_rgba = pic.rgba;
        p.avatar_w = pic.w;
        p.avatar_h = pic.h;
        p.avatar_id = pic.id;
    }
}

void PlayerScreen::feedPerformanceHud() {
    static char l_video[192], l_audio[192];
    static char l_perf[160], l_queues[160], l_clocks[160];

    char vcodec[48] = "NONE", acodec[48] = "NONE", alang[24] = "und";
    int vw = 0, vh = 0, ach = 0, arate = detected_audio_rate;

    if (play_fmt && video_stream_index >= 0 && video_stream_index < (int)play_fmt->nb_streams) {
        AVCodecParameters *cp = play_fmt->streams[video_stream_index]->codecpar;
        if (cp) {
            std::snprintf(vcodec, sizeof(vcodec), "%s", avcodec_get_name(cp->codec_id));
            vw = cp->width;
            vh = cp->height;
        }
    }
    if (play_fmt && audio_stream_index >= 0 && audio_stream_index < (int)play_fmt->nb_streams) {
        AVStream *st = play_fmt->streams[audio_stream_index];
        if (st && st->codecpar) {
            std::snprintf(acodec, sizeof(acodec), "%s", avcodec_get_name(st->codecpar->codec_id));
            ach = st->codecpar->ch_layout.nb_channels;
            arate = st->codecpar->sample_rate;
            const AVDictionaryEntry *lang = av_dict_get(st->metadata, "language", nullptr, 0);
            if (lang && lang->value && lang->value[0]) {
                std::snprintf(alang, sizeof(alang), "%s", lang->value);
            }
        }
    }

    const char *backend = (evo_pb_active_backend() == EVO_VDEC_BACKEND_NATIVE)
                              ? "Hardware (sceVideodec2)" : "Software (FFmpeg)";

    std::snprintf(l_video, sizeof(l_video), "VIDEO  %s  %dx%d  %.3f fps  (%s)",
                  vcodec, vw, vh, evo_pb_video_fps(), backend);
    std::snprintf(l_audio, sizeof(l_audio), "AUDIO  %s  %s  %dch  %d Hz",
                  acodec, alang, ach, arate);
    std::snprintf(l_perf, sizeof(l_perf), "RENDER %d fps  /  DECODE %d fps  /  %s",
                  perf_render_fps, perf_decode_fps, evo_hw_model_name());
    std::snprintf(l_queues, sizeof(l_queues), "QUEUES  video %d/96  audio %d/96  pcm %d",
                  packet_queue_count(&video_packet_queue),
                  packet_queue_count(&audio_packet_queue),
                  audio_queue_count);
    std::snprintf(l_clocks, sizeof(l_clocks), "CLOCKS  video %.2f  audio %.2f  delta %.3f",
                  evo_pb_video_clock_s(), audio_clock_seconds,
                  evo_pb_video_clock_s() - audio_clock_seconds);

    evo_perf_snapshot_t s;
    evo_perf_monitor_get(&s);

    evo_perf_hud_t hud;
    std::memset(&hud, 0, sizeof(hud));
    hud.line_video = l_video;
    hud.line_audio = l_audio;
    hud.line_perf = l_perf;
    hud.line_queues = l_queues;
    hud.line_clocks = l_clocks;
    hud.hist_len = EVO_PERF_HIST;
    hud.gpu_hist = s.gpu_hist;
    hud.ram_hist = s.ram_hist;
    hud.cpu_hist = s.cpu_hist;
    hud.gpu_pct = s.gpu_pct;
    hud.gpu_peak_pct = s.gpu_peak_pct;
    hud.ram_mb = s.ram_mb;
    hud.ram_peak_mb = s.ram_peak_mb;
    hud.ram_total_mb = s.ram_total_mb;
    hud.cpu_pct = s.cpu_pct;
    hud.cpu_peak_pct = s.cpu_peak_pct;

    evo_rmlui_update_perf_hud(&hud);
}

void PlayerScreen::render(uint32_t* framebuffer, int width, int height) {
    auto playback = Application::getInstance().getPlaybackController();
    auto metaService = Application::getInstance().getMediaMetadataService();
    if (!playback) return;

    /* How chat is drawn this frame. Application put the video in the box for
     * this mode before the blit (evo_twitch_chat_mode.h); the OSD goes into
     * the same box. */
    const bool twitchStream = twitchPlaying(playback);
    const bool twitchVideo = !twitchStream && twitchVideoPlaying(playback);
    const twitch::ChatMode chatMode = evo_twitch::chat_mode_drawn(twitchStream);
#if defined(EVO_AGC_DEVICE)
    {
        float ax, ay, aw, ah;
        evo_twitch::chat_video_box(chatMode, &ax, &ay, &aw, &ah);
        evo_hui_set_osd_area(ax, ay, aw, ah);
    }
#endif

    static int s_player_screen_log = 5;
    if (s_player_screen_log > 0) {
        s_player_screen_log--;
        evo_boot_log("PlayerScreen::render fb=%p %dx%d alpha=%d rml_init=%d",
                     (void*)framebuffer, width, height, m_osdVisibilityAlpha,
                     evo_rmlui_is_initialized());
        evo_boot_log_flush();
    }

    /*
     * Both branches only matter to the CPU rasteriser. In AGC mode RmlUi draws
     * through the GPU and never reads this buffer back, so the music-mode fill
     * was 8.3 MB of stores a frame that nothing looked at -
     * draw_video_frame_to_fb has already been an empty function since the GL
     * quad took over.
     */
    if (evo_rmlui_blit_mode()) {
        if (!playback->isMusicMode()) {
            draw_video_frame_to_fb(framebuffer, 0, 0, width, height);
        } else {
            std::fill_n(framebuffer, width * height, 0xFF000000);
        }
    }

    /* A video's timeline sits where toasts go: they keep above it while the
     * controls are up ("Resumed at 1:23:45" comes with them). */
    toast_set_bottom(m_osdVisibilityAlpha > 0 && twitchVideo && !playback->isLiveSource() ? kToastOverTimeline
                                                                                           : 0);

    if (m_osdVisibilityAlpha > 0 && evo_rmlui_is_initialized()) {
        std::string titleStr, metaStr;
        /*
         * #90 / #9: a provider supplies its own title, and deriving one from
         * the source string is what put a URL with a query string on the OSD.
         * cleanMediaTitle takes the last path component and tidies it, which is
         * exactly right for /mnt/usb0/Movie.2019.mkv and exactly wrong for
         * http://host/emby/Videos/abc/stream?api_key=...
         */
        titleStr = playback->getDisplayTitle();
        if (titleStr.empty() && metaService) {
            metaService->cleanMediaTitle(playback->getCurrentFilePath(), titleStr, metaStr);
        }

        evo_playback_osd_params_t p;
        std::memset(&p, 0, sizeof(p));
        p.title = titleStr.empty() ? "Video Playback" : titleStr.c_str();
        p.metadata = metaStr.c_str();
        /*
         * A rebuffer parks both decode threads mid-playback, so the picture
         * freezes on its last frame - which reads as a hung player rather
         * than a short link. Say which it is, with the spinner.
         */
        p.status = readerStalled() ? "Waiting for the server" : pb_prebuffer_hold ? "Buffering" : "";
        p.is_live = playback->isLiveSource() ? 1 : 0;
        p.viewers = -1;
        if (twitchStream || twitchVideo)
            fillTwitchOsd(p, playback->currentSource(), osdChatMode(chatMode), twitchVideo);
        p.position_sec = playback->getPositionSeconds();
        p.duration_sec = playback->getDurationSeconds();
        p.percentage = playback->getPercentage();
        p.paused = playback->isPaused() ? 1 : 0;
        p.scrub_active = playback->isScrubbing() ? 1 : 0;
        p.loading = (!playback->isMusicMode() &&
                     (!video_frame_loaded || g_pp_pb.seek_discarding)) ? 1 : 0;
        p.scrub_target = playback->getScrubTargetSeconds();
        p.view_mode = static_cast<int>(playback->getViewMode());
        p.show_stats = m_showStatsForNerds ? 1 : 0;
        p.can_stats = kStatsPanel ? 1 : 0;
        p.alpha = m_osdVisibilityAlpha;
        p.fps = perf_render_fps;
        p.debug_overlay = evo_fps_counter_enabled();
        p.music_mode = playback->isMusicMode() ? 1 : 0;
        p.clip_card = m_clipCardUntilMs != 0 ? 1 : 0;

        evo_rmlui_update_playback_params(&p);
        if (m_showStatsForNerds) {
            feedPerformanceHud();
        }
        evo_rmlui_render_playback_osd(framebuffer, width, height);
    }

    /* Twitch chat: beside the video or over it, and over the OSD. */
    if (twitchStream) {
        if (!m_chatHintChecked) {
            m_chatHintChecked = true;
            if (!evo_twitch::settings().chatHintSeen) {
                /* If the data folder isn't open yet the save fails and the
                 * hint comes again next launch. */
                evo_twitch::set_chat_hint_seen(true);
                evo_twitch::save_settings(evo_twitch::settings());
                toast("Chat", "R1 moves the chat or turns it off");
            }
        }
    }
    const PlaybackSource* src = twitchStream ? playback->currentSource() : nullptr;
    const int chatKitMode = (!src || chatMode == twitch::ChatMode::Off) ? -1
                          : chatMode == twitch::ChatMode::Side ? EVO_HUI_TWITCH_CHAT_SIDE
                                                               : EVO_HUI_TWITCH_CHAT_OVERLAY;
    if (chatKitMode >= 0)
        renderChat(chatKitMode, src->title, src->item_id, width, height);

    /* The clip card, over the video, the OSD and chat. */
    if (m_clipCardUntilMs != 0 || m_clipCardFadeFrames > 0)
        renderClipCard(static_cast<int>(chatMode), width, height);

    /* The player doesn't clear between video frames, so chat drawn over a
     * letterbox bar would stay there once it stops being drawn (over the
     * video, then off). After any change both buffers get one clear, on the
     * next two frames drawn; a new video box does that by itself, this
     * covers the rest. */
    if (chatKitMode != m_chatDrawnMode) {
        m_chatDrawnMode = chatKitMode;
        m_chatClearFrames = 2;
    }
    /* The same when EVO's own keyboard opens or closes over the video. While
     * it is up, hasActiveOverlay() marks every frame. */
    const bool keyboardOverVideo = virtualKeyboardUp();
    if (keyboardOverVideo != m_keyboardOverVideo) {
        m_keyboardOverVideo = keyboardOverVideo;
        m_chatClearFrames = 2;
    }
    if (m_chatClearFrames > 0) {
        --m_chatClearFrames;
        evo_agc_runtime_note_ui_drawn();
    }
}

void PlayerScreen::syncChatUrls(bool badgesKnown) {
    const evo_twitch::ChatView& view = evo_twitch::chat_view();
    const std::deque<twitch::ChatLine>& lines = view.log->lines();
    const std::size_t n = std::min(lines.size(), kChatLines);
    const std::size_t first = lines.size() - n;

    /* Both run oldest first with rising serials: keep the entries of the
     * lines still drawn, build the new ones, and let the rest go (with the
     * next sync's clear). Moving an entry into the reserved spare allocates
     * nothing. */
    m_chatUrlsSpare.clear();
    m_chatUrlsSpare.reserve(kChatLines);
    std::size_t old = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const twitch::ChatLine& line = lines[first + i];
        while (old < m_chatUrls.size() && m_chatUrls[old].serial < line.serial) ++old;
        if (old < m_chatUrls.size() && m_chatUrls[old].serial == line.serial) {
            m_chatUrlsSpare.push_back(std::move(m_chatUrls[old]));
            ++old;
        } else {
            m_chatUrlsSpare.emplace_back();
            ChatLineUrls& u = m_chatUrlsSpare.back();
            u.serial = line.serial;
            u.color = chatColor(line.color);
            u.mention = twitch::mentions(line, m_chatLogin);
            u.emotes.reserve(line.segments.size());
            /* Twitch's own emotes by id; 7TV's, BetterTTV's and FFZ's come
             * with their picture's address. */
            for (const twitch::ChatSegment& seg : line.segments)
                u.emotes.push_back(!seg.emote                ? std::string()
                                   : !seg.emoteId.empty()    ? twitch::emoteImageUrl(seg.emoteId, kChatEmoteScale)
                                                             : seg.imageUrl);
        }
        /* The badge list arrives after the first lines: until it does a line
         * has none, and the kit lays it out again when they come. */
        ChatLineUrls& u = m_chatUrlsSpare.back();
        if (badgesKnown && !u.badgesKnown) {
            u.badges.clear();
            for (const twitch::Badge& b : line.badges) {
                std::string url = view.badges->url(b);
                if (!url.empty()) u.badges.push_back(std::move(url));
            }
            u.badgesKnown = true;
        }
    }
    m_chatUrls.swap(m_chatUrlsSpare);
}

void PlayerScreen::renderChat(int kitMode, const std::string& title, const std::string& login, int width,
                              int height) noexcept {
#if defined(EVO_AGC_DEVICE)
    try {
        const evo_twitch::ChatView& view = evo_twitch::chat_view();
        if (!view.log || !view.badges) return;

        /* Mentions are found for whoever is logged in; another login (or
         * none) finds them afresh for every line. */
        const evo_twitch::AuthView& auth = evo_twitch::auth_view();
        const bool loggedIn = auth.state == evo_twitch::AuthState::LoggedIn;
        if (loggedIn ? auth.login != m_chatLogin : !m_chatLogin.empty()) {
            if (loggedIn)
                m_chatLogin = auth.login;
            else
                m_chatLogin.clear();
            m_chatUrls.clear();
            m_chatSynced = false;
        }

        const bool badgesKnown = !view.badges->empty();
        if (!m_chatSynced || view.log->revision() != m_chatRevision || badgesKnown != m_chatBadgesKnown) {
            m_chatSynced = false;
            syncChatUrls(badgesKnown);
            m_chatRevision = view.log->revision();
            m_chatBadgesKnown = badgesKnown;
            m_chatSynced = true;
        }

        /* m_chatUrls holds the newest lines, one entry each, in order; the
         * kit gets the newest few of them. */
        const std::deque<twitch::ChatLine>& lines = view.log->lines();
        if (m_chatUrls.size() > lines.size()) return;
        const std::size_t n = std::min(m_chatUrls.size(),
                                       kitMode == EVO_HUI_TWITCH_CHAT_SIDE ? kChatLinesSide : kChatLinesOverlay);
        const std::size_t firstUrl = m_chatUrls.size() - n;
        const std::size_t firstLine = lines.size() - n;

        std::size_t segmentTotal = 0, badgeTotal = 0;
        for (std::size_t k = 0; k < n; ++k) {
            segmentTotal += lines[firstLine + k].segments.size();
            badgeTotal += m_chatUrls[firstUrl + k].badges.size();
        }
        /* Within their capacity once warm: no allocation. */
        m_chatLines.resize(n);
        m_chatSegments.resize(segmentTotal);
        m_chatBadgePics.resize(badgeTotal);

        /* The lines, oldest first, as the kit takes them. */
        std::size_t seg = 0, badge = 0;
        for (std::size_t k = 0; k < n; ++k) {
            const twitch::ChatLine& line = lines[firstLine + k];
            const ChatLineUrls& u = m_chatUrls[firstUrl + k];
            evo_hui_twitch_chat_line_t& out = m_chatLines[k];
            out.serial = line.serial;
            out.kind = chatKind(line);
            out.name = line.name.c_str();
            out.login = line.user.c_str();
            out.color = u.color;
            out.system_text = line.systemText.c_str();
            out.badges = m_chatBadgePics.data() + badge;
            out.badge_count = static_cast<int>(u.badges.size());
            badge += u.badges.size();
            out.segments = m_chatSegments.data() + seg;
            out.segment_count = static_cast<int>(line.segments.size());
            out.reply_to = line.replyTo.c_str();
            out.reply_login = line.replyToLogin.c_str();
            out.mention = u.mention ? 1 : 0;
            for (std::size_t j = 0; j < line.segments.size(); ++j) {
                evo_hui_twitch_chat_segment_t& o = m_chatSegments[seg++];
                o.emote = (j < u.emotes.size() && !u.emotes[j].empty()) ? 1 : 0;
                o.text = line.segments[j].text.c_str();
                o.pic = evo_hui_twitch_image_t{};
            }
        }

        /* Their pictures, newest line first: the image service fetches in
         * the order it is asked, and the newest lines are the ones surely on
         * screen. Asked every frame (that keeps them in memory); good until
         * this frame's render has returned. */
        for (std::size_t k = n; k-- > 0;) {
            const ChatLineUrls& u = m_chatUrls[firstUrl + k];
            const evo_hui_twitch_chat_line_t& out = m_chatLines[k];
            const std::size_t b0 = static_cast<std::size_t>(out.badges - m_chatBadgePics.data());
            for (std::size_t b = 0; b < u.badges.size(); ++b)
                m_chatBadgePics[b0 + b] = twitch_screen::picture(u.badges[b], 0, kChatBadgeH);
            const std::size_t s0 = static_cast<std::size_t>(out.segments - m_chatSegments.data());
            for (std::size_t j = 0; j < static_cast<std::size_t>(out.segment_count); ++j) {
                evo_hui_twitch_chat_segment_t& o = m_chatSegments[s0 + j];
                if (o.emote) o.pic = twitch_screen::picture(u.emotes[j], 0, kChatEmoteH);
            }
        }

        /* The side panel's header: the channel's display name. The launcher
         * titles a Twitch stream "<name> · <stream title>", or just "<name>"
         * when there is no title to show (evo_twitch_launcher.cpp,
         * playerTitle). Reused, so no allocation once warm. */
        const std::size_t dot = title.find(" \xC2\xB7 ");
        if (!title.empty())
            m_chatChannel.assign(title, 0, dot);
        else
            m_chatChannel.assign(login);

        evo_hui_twitch_chat_params_t p;
        std::memset(&p, 0, sizeof(p));
        p.mode = kitMode;
        /* Where the last blit really put the picture, in canvas units. Before
         * the first blit in this layout it is unknown (video_w 0), and the
         * kit takes a 16:9 picture in the side box, or the whole screen over
         * the video. */
        int vx, vy, vw, vh, pw, ph;
        if (evo_agc_runtime_get_video_px(&vx, &vy, &vw, &vh, &pw, &ph) && pw > 0 && ph > 0) {
            const float sx = EVO_CHAT_CANVAS_W / static_cast<float>(pw);
            const float sy = EVO_CHAT_CANVAS_H / static_cast<float>(ph);
            p.video_x = static_cast<float>(vx) * sx;
            p.video_y = static_cast<float>(vy) * sy;
            p.video_w = static_cast<float>(vw) * sx;
            p.video_h = static_cast<float>(vh) * sy;
        }
        p.channel = m_chatChannel.c_str();
        p.lines = n ? m_chatLines.data() : nullptr;
        p.line_count = static_cast<int>(n);
        switch (evo_twitch::settings().chatTextSize) {
            case twitch::ChatTextSize::Small: p.text_size = EVO_HUI_TWITCH_CHAT_TEXT_SMALL; break;
            case twitch::ChatTextSize::Large: p.text_size = EVO_HUI_TWITCH_CHAT_TEXT_LARGE; break;
            default:                          p.text_size = EVO_HUI_TWITCH_CHAT_TEXT_MEDIUM; break;
        }
        evo_hui_twitch_chat_update(&p);
        /* 0 when the kit isn't up: then there is simply no chat. */
        evo_rmlui_render_kit_screen(evo_hui_render_twitch_chat, width, height);
    } catch (...) {
        /* Out of memory: no chat this frame, and every line built afresh
         * next time (a failed sync can leave entries half moved). */
        m_chatUrls.clear();
        m_chatUrlsSpare.clear();
        m_chatSynced = false;
    }
#else
    (void)kitMode;
    (void)title;
    (void)login;
    (void)width;
    (void)height;
#endif
}

} // namespace evo
