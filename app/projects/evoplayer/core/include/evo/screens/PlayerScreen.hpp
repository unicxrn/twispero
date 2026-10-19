#ifndef EVO_PLAYER_SCREEN_HPP
#define EVO_PLAYER_SCREEN_HPP

#include "evo/screens/StatefulScreen.hpp"
#include "evo/fsm/StateMachine.hpp"
#include "evo_hui_twitch.h"
#include "evo_rmlui_bridge.h"
#include "twitch/chat_log.hpp"

#include <deque>
#include <string>
#include <vector>

namespace evo {

class IPlaybackController;
struct PlaybackSource;

/**
 * @brief Discrete UI states for the media player presentation layer.
 */
enum class PlayerScreenState : int {
    NormalPlayback = 0,
    OsdVisible = 1,
    Scrubbing = 2,
    StatsOverlay = 3
};

/**
 * @brief Discrete trigger events for the player presenter state machine.
 */
enum class PlayerScreenEvent : int {
    ShowOsd = 0,
    HideOsd = 1,
    StartScrub = 2,
    EndScrub = 3,
    ToggleStats = 4
};

class PlayerScreen : public StatefulScreen {
public:
    PlayerScreen();
    ~PlayerScreen() override = default;

    // --- IStatefulFeature ---
    IStateMachine* getStateMachine() override { return &m_playerFsm; }
    const IStateMachine* getStateMachine() const override { return &m_playerFsm; }

    PlayerScreenState getPlayerState() const {
        return m_playerFsm.getCurrentState();
    }

    ScreenId getScreenId() const override { return ScreenId::Player; }
    void onEnter() override;
    void onExit() override;
    bool handleInput(uint32_t pressed, uint32_t held, uint32_t released) override;
    void update(double deltaMs) override;
    void render(uint32_t* framebuffer, int width, int height) override;

    bool isStatsForNerdsVisible() const { return m_showStatsForNerds; }
    void toggleStatsForNerds();
    bool hasActiveOverlay() const;

private:
    void initPlayerStateMachine();
    void feedPerformanceHud();
    /* Asks the data service for the channel playing again once a minute
     * (evo_twitch_data.h, now_playing()), for the OSD's viewer count; for a
     * video or clip once, for the channel's picture when its card had none. */
    void refreshNowPlaying(IPlaybackController* playback);
    /* The Twitch half of the OSD's params: the channel, the quality playing
     * and the keys for chat in this chat mode (EVO_OSD_CHAT_*); for a video
     * or clip (`video`), its line ("Past broadcast · 3 days ago") and no chat. */
    void fillTwitchOsd(evo_playback_osd_params_t& p, const PlaybackSource* src, int chatMode, bool video);
    /* Twitch chat in the kit's mode (EVO_HUI_TWITCH_CHAT_SIDE or _OVERLAY),
     * for the stream with this title and login. */
    void renderChat(int kitMode, const std::string& title, const std::string& login, int width,
                    int height) noexcept;
    /* Writes an R1 change that is waiting (m_chatSavePending). */
    void flushChatSave();
    /* Triangle on a Twitch stream: the keyboard for a chat message, or a
     * toast saying why not. replyId, when set, sends it as a reply to that
     * line and titles the keyboard for replyName. */
    void openChatKeyboard(IPlaybackController* playback, const std::string& replyId = {},
                          const std::string& replyName = {});
    /* Up/Down walk the chat lines that can be replied to (dir -1 is older);
     * the first Up pauses chat, Down past where it paused lets it go on. */
    void moveChatReply(int dir);
    /* L1: with nothing picked, pauses chat on its newest line; with a line
     * picked, the keyboard to reply to it. */
    void replyToChat(IPlaybackController* playback);
    /* Keeps a copy of the lines on screen now (m_pausedLines) and says so. */
    void pauseChat(const std::deque<twitch::ChatLine>& live);
    /* Pauses chat and picks its newest line that can be replied to. False,
     * and chat runs on, when there is none. */
    bool startChatPick();
    /* Marks `line` (in m_pausedLines) as the one to reply to. */
    void pickChatLine(const twitch::ChatLine& line);
    /* No line picked: chat goes on from its newest line. */
    void resumeChat();
    /* Chat moved to another channel: it goes on. */
    void checkReplyChannel();
    /* Every frame: chat goes on once the channel changed, or nothing was
     * pressed for kChatReplyIdleMs. */
    void tickChatReply();
    /* One past the last line to draw of `lines`: all of them, or while a
     * line is picked, up to kChatReplyBelow under it. */
    std::size_t chatWindowEnd(const std::deque<twitch::ChatLine>& lines) const;
    /* The keyboard's confirm: hands the message to the chat service.
     * evo_keyboard calls it on the main thread. */
    static void OnChatSubmitted(const char* text, void* userdata) noexcept;
    /* Brings m_chatUrls in line with `lines` before `end` (the log's, or the
     * paused copy). */
    void syncChatUrls(bool badgesKnown, const std::deque<twitch::ChatLine>& lines, std::size_t end);
    /* Square on a live Twitch stream: asks the clip maker for a clip
     * (evo_twitch_clip.h) and toasts "Making a clip...", or why not. */
    void makeClip(IPlaybackController* playback);
    /* The clip maker's outcome, when there is one: the card with the clip's
     * QR, or a toast. */
    void takeClipResult();
    /* The card goes: it fades out over the next frames drawn. */
    void closeClipCard();
    /* The card, while it shows or fades out, in the OSD's area for this chat
     * mode. */
    void renderClipCard(int chatMode, int width, int height) noexcept;

    StateMachine<PlayerScreenState, PlayerScreenEvent> m_playerFsm;
    int m_osdVisibilityAlpha = 255;
    uint64_t m_controlsLastUsedMs = 0;
    bool m_stallToastShown = false;   /* "connection lost" is up; say when it is back */
    uint64_t m_lastPlayPauseToggleMs = 0;
    bool m_showStatsForNerds = false;

    /* Chat. What the chat log can't hand the kit as it is: the picture URLs
     * and the name's colour, worked out once per line (keyed by its serial)
     * and kept while the line is among the newest drawn. Everything here is
     * reused frame after frame, so a frame with nothing new allocates
     * nothing. */
    struct ChatLineUrls {
        unsigned long long serial = 0;
        bool badgesKnown = false;         /* built after the badge list arrived */
        bool mention = false;             /* it mentions m_chatLogin */
        unsigned int color = 0xFFFFFF;    /* 0xRRGGBB */
        std::vector<std::string> badges;  /* the badges that have a picture */
        std::vector<std::string> emotes;  /* one per segment, "" for text */
    };
    std::vector<ChatLineUrls> m_chatUrls;       /* the lines drawn, oldest first */
    std::vector<ChatLineUrls> m_chatUrlsSpare;  /* swapped with m_chatUrls on a change */
    unsigned long long m_chatRevision = 0;
    bool m_chatBadgesKnown = false;
    bool m_chatSynced = false;
    unsigned long long m_chatEndSerial = 0;  /* the last line synced (chatWindowEnd) */
    std::vector<evo_hui_twitch_chat_line_t> m_chatLines;
    std::vector<evo_hui_twitch_chat_segment_t> m_chatSegments;
    std::vector<evo_hui_twitch_image_t> m_chatBadgePics;
    bool m_chatHintChecked = false;  /* the "R1 changes the chat" hint, once a run */
    std::string m_chatChannel;       /* the side panel's header */
    std::string m_chatLogin;         /* the logged-in user the mentions were found for, "" logged out */
    int m_chatDrawnMode = -1;        /* EVO_HUI_TWITCH_CHAT_*, -1 when no chat was drawn */
    int m_chatClearFrames = 0;       /* frames left to mark for a clear after a change */
    bool m_chatSavePending = false;  /* R1 changed the mode; the file isn't written yet */
    uint64_t m_chatSaveDueMs = 0;    /* written then, unless R1 comes again */
    std::string m_chatSendChannel;   /* the login the open keyboard writes to */
    std::string m_chatReplyId;       /* set while the open keyboard is a reply */
    std::string m_replyChannel;      /* chat channel the highlight belongs to */
    unsigned long long m_replySerial = 0; /* the line picked to reply to, 0 for none: chat runs */
    std::deque<twitch::ChatLine> m_pausedLines; /* the lines on screen when chat paused */
    uint64_t m_replyUsedMs = 0;           /* the last Up, Down or L1 on it */
    bool m_keyboardOverVideo = false; /* EVO's own keyboard was up last frame drawn */

    /* The card for a clip just made (Square): its page as a QR, shown for
     * evo_twitch::kClipCardMs or until Circle. */
    std::vector<unsigned char> m_clipQr;
    int m_clipQrSize = 0;
    uint64_t m_clipCardUntilMs = 0;  /* NowMs() when it goes; 0 when it isn't up */
    int m_clipCardFadeFrames = 0;    /* frames still drawn while it fades out */

    /* The OSD's channel details. */
    unsigned long long m_nowPlayingStamp = 0;  /* evo_twitch_now_playing()'s, when last seen */
    unsigned long long m_nowPlayingDueMs = 0;  /* the next ask (data_now_ms()'s clock) */
    char m_osdQuality[48] = {};                /* the quality pill's text */
    std::string m_osdVideoMeta;                /* a video's or clip's line, built per stamp */
    unsigned long long m_osdVideoMetaStamp = 0;
};

} // namespace evo

#endif // EVO_PLAYER_SCREEN_HPP
