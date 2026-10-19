#ifndef EVO_I_PLAYBACK_CONTROLLER_HPP
#define EVO_I_PLAYBACK_CONTROLLER_HPP

#include "evo/Common.hpp"
#include "evo/interfaces/IStatefulFeature.hpp"
#include <string>
#include <vector>

namespace evo {

/**
 * @brief Discrete playback engine states.
 */
enum class PlaybackState : int {
    Stopped = 0,
    Opening = 1,
    Playing = 2,
    Paused = 3,
    Scrubbing = 4,
    Seeking = 5,
    Finished = 6,
    Error = 7
};

/**
 * @brief Discrete playback trigger events.
 */
enum class PlaybackEvent : int {
    Open = 0,
    Play = 1,
    Pause = 2,
    Resume = 3,
    StartScrub = 4,
    MoveScrub = 5,
    ConfirmScrub = 6,
    CancelScrub = 7,
    Seek = 8,
    Finish = 9,
    Stop = 10,
    Fail = 11
};

/**
 * @brief Where the thing being played came from, and what to call it.
 *
 * #90. A bare path string was enough while every source was a file on the USB
 * stick: the filename was the identity, the title and the thing FFmpeg opened
 * all at once. A provider item is none of those - its URL carries an auth
 * token, its title is metadata the service returned, and its identity is an
 * opaque id that outlives the URL.
 *
 * Conflating them is the root cause of #9 (the OSD showing a URL instead of a
 * title) and of a token ending up in Recent, where it is both a leak and
 * useless: a signed URL that has expired is not a thing you can replay.
 */
struct PlaybackSource {
    /** What avformat actually opens. May carry a token; never displayed, and
     *  never persisted. */
    std::string url;
    /** What the OSD shows. For a local file this is left empty and the title
     *  is derived from the filename as before. */
    std::string title;
    /** Who the stream comes from ("twitch", "twitch-vod", "twitch-clip"), or
     *  empty for a local file. */
    std::string provider;
    /** Opaque provider item id, or empty for a local file. */
    std::string item_id;
    /** No duration, not seekable: the OSD, the resume store and the seek path
     *  all assume otherwise, so this has to be known rather than discovered
     *  by a seek that fails. */
    bool is_live = false;
    /** A recording: a playlist still being written (a Twitch past broadcast
     *  of a stream that is still on) plays from its first segment, not from
     *  near its live edge. */
    bool from_start = false;
    /** Nobody asked for it (Home's live picture): a start that fails is
     *  logged, not toasted. */
    bool quiet = false;

    /** A non-local source: a network stream, not a file on the console. */
    bool isProvider() const { return !provider.empty(); }
};

/**
 * @brief Interface for playback session management, A/V synchronization, scrubbing, and transport controls.
 *
 * Implements IStatefulFeature to guarantee state integrity across media transitions.
 */
class IPlaybackController : public IStatefulFeature {
public:
    virtual ~IPlaybackController() = default;

    virtual bool startPlayback(const std::string& filePath, double resumeOffset = 0.0) = 0;
    /** #90: the general form. startPlayback() is this with a local-file
     *  source, and every caller that has more than a path should use this. */
    virtual bool startPlaybackSource(const PlaybackSource& source,
                                     double resumeOffset = 0.0) = 0;
    virtual void stopPlayback() = 0;

    /** The title to put on screen: the provider's, when there is one, else
     *  empty so the caller falls back to deriving it from the filename. */
    virtual const std::string& getDisplayTitle() const = 0;
    virtual bool isLiveSource() const = 0;
    /** What is playing, or nullptr when nothing is: stopped, still opening,
     *  or an open that failed. Main thread only, like the rest of this
     *  interface; the pointer is good until playback next starts or stops. */
    virtual const PlaybackSource* currentSource() const = 0;

    virtual void togglePause() = 0;
    virtual void setPaused(bool paused) = 0;
    virtual bool isPaused() const = 0;
    virtual bool isActive() const = 0;
    virtual bool isMusicMode() const = 0;
    /** The stream's first picture is on screen, or it has none to wait for
     *  (audio only). Paused before that, the screen stays black: the decode
     *  thread decodes nothing while paused. False while nothing plays. */
    virtual bool isPictureReady() const = 0;

    virtual PlaybackState getPlaybackState() const = 0;

    virtual double getPositionSeconds() const = 0;
    virtual double getDurationSeconds() const = 0;
    virtual double getPercentage() const = 0;
    virtual const std::string& getCurrentFilePath() const = 0;

    virtual void seekTo(double targetSeconds) = 0;
    virtual void beginScrub() = 0;
    virtual void moveScrub(double deltaSeconds) = 0;
    virtual bool confirmScrub() = 0;
    virtual void cancelScrub() = 0;
    virtual bool isScrubbing() const = 0;
    virtual double getScrubTargetSeconds() const = 0;
    virtual void updateScrubHold(uint32_t heldButtons) = 0;
    virtual void tickScrubAutoCommit() = 0;

    virtual void jumpChapter(int direction) = 0;

    virtual void cycleViewMode() = 0;
    virtual ViewMode getViewMode() const = 0;
    virtual void setViewMode(ViewMode mode) = 0;
};

} // namespace evo

#endif // EVO_I_PLAYBACK_CONTROLLER_HPP
