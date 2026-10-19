#pragma once
/*
 * evo_twitch.h - the Twitch module's entry points for EVO's frame loop.
 *
 * The definitions are C++ (twitch/src) and noexcept: an exception never
 * crosses into EVO. C++ sees the noexcept here too, because a function's
 * declarations must all agree on it.
 */
#ifdef __cplusplus
#define EVO_TWITCH_NOEXCEPT noexcept
extern "C" {
#else
#define EVO_TWITCH_NOEXCEPT
#endif

/* Runs once at boot. Throws and catches a C++ exception across translation
 * units and parses a small JSON document with a float. Logs one line:
 * "twitch: self-test ok" or "twitch: self-test FAILED: <reason>".
 * Returns 1 on success, 0 on failure. Never lets an exception escape. */
int evo_twitch_selftest(void) EVO_TWITCH_NOEXCEPT;

/* The boot autoplay, a test switch. On a worker thread it waits for the data
 * folder (up to 10 s) and plays only when <data root>/twitch_autoplay exists
 * (any content): the channel named in <data root>/twitch_channel.txt (first
 * line, trimmed), or the most-watched live stream when that file is missing
 * or empty. Without the switch, or when the data folder is still closed after
 * 10 s, it logs "autoplay off" and does nothing else that launch.
 * Call once, after the first screen is up: the stream only starts if the
 * user is still on that screen with nothing playing, so leaving it drops the
 * autoplay (toast "Autoplay skipped"). Later calls do nothing. If the
 * self-test failed it shows the toast "Self-test failed, see the log" and
 * does nothing else. Main thread only. */
void evo_twitch_launcher_start(void) EVO_TWITCH_NOEXCEPT;

/* Plays a channel the user picked: looks it up on a worker thread (channel
 * details, token, the default quality from Settings), then starts it like the
 * autoplay and opens the player; once it plays, the channel goes first in
 * the watch history (evo_twitch_history.h), as an autoplayed one does. That channel or an error toast ("This channel is offline",
 * "Not found", "No connection", ...), never another channel.
 * One at a time: while a lookup or start is in flight, including an autoplay
 * that found its switch file, a request is turned down with the toast
 * "Already starting a stream". An autoplay check that has not found the file
 * yet stands down instead, and the request goes next.
 * Other sources win: a request is turned down with "Something else is
 * playing" or "Something else is starting" while a playback screen is open
 * or while the player holds anything but an error or a finished file (those
 * are stopped first). The same check runs again just before the start.
 * Ignored while the app is quitting. Main thread only. */
void evo_twitch_play(const char* login) EVO_TWITCH_NOEXCEPT;

/* Home's small muted picture of the top live channel (see the launcher's
 * file header). It starts only while the launcher is idle, nothing else
 * plays and Home is up, in a low quality (480p or the closest below), with
 * no sound, no toasts and no chat. Leaving Home stops it. It doesn't count
 * as "Starting" or as "something else is playing": a play request stops it
 * and goes ahead, and one made while it is still opening waits for that.
 * One that fails, or dies once up, is dropped and holds the next try back
 * (15 s, doubling to 5 minutes; 10 minutes for that channel). Calling it
 * every frame is fine. Main thread only. */
void evo_twitch_preview(const char* login) EVO_TWITCH_NOEXCEPT;
void evo_twitch_preview_stop(void) EVO_TWITCH_NOEXCEPT;
/* 1 when that login's picture is the one on Home and a frame is ready. */
int  evo_twitch_preview_picture(const char* login) EVO_TWITCH_NOEXCEPT;
/* 1 while that picture is up. The frame loop uses it to draw the video. */
int  evo_twitch_preview_active(void) EVO_TWITCH_NOEXCEPT;
/* 1 while it is looked up or opening. busy() may be 1 then too, but the pad
 * may still reach the screens: everything they can do waits for it. */
int  evo_twitch_preview_starting(void) EVO_TWITCH_NOEXCEPT;
/* The picture is this login, plays already, and in the quality Settings
 * would pick: unmute, remember the channel and open the player, without
 * starting the stream again. 1 when it did that; 0 and the caller plays the
 * channel as usual. */
int  evo_twitch_preview_take(const char* login) EVO_TWITCH_NOEXCEPT;

/* ---- Videos and clips (stage 7) ---- */

/* What kind of video plays: Twitch's three kinds, and clips. 0 is a live
 * stream (evo_twitch_now_playing_t's video_type). */
#define EVO_TWITCH_VIDEO_ARCHIVE   1   /* a past broadcast */
#define EVO_TWITCH_VIDEO_HIGHLIGHT 2
#define EVO_TWITCH_VIDEO_UPLOAD    3
#define EVO_TWITCH_VIDEO_CLIP      4

/* What the screen that asked knows of a video or clip, from its card: the
 * player's OSD shows it, and a video's saved progress keeps it for Home's
 * "Continue watching". Every string may be NULL or "" when not known. Copied
 * in the call. */
typedef struct {
    int         type;           /* EVO_TWITCH_VIDEO_* */
    const char* title;
    const char* channel_login;
    const char* channel_name;   /* display name */
    const char* category;
    const char* age;            /* "3 days ago" */
    const char* published;      /* ISO 8601, kept with a video's progress (ages it later) */
    const char* curator;        /* a clip's: who made it */
    const char* thumb_url;      /* the 440x248 thumbnail, kept with a video's progress */
    const char* avatar_url;     /* the channel's picture */
    int         seconds;        /* its length, 0 when not known */
    int         views;          /* -1 when not known */
} evo_twitch_video_info_t;

/* Plays a past broadcast, highlight or upload by its video id, or a clip by
 * its slug: Cross on a channel page's Videos and Clips tabs, on a category's
 * Clips and on Home's Continue watching. `info` may be NULL. Like
 * evo_twitch_play(): a worker looks it up (a video: its token and playlist,
 * in the default quality from Settings; a clip: its signed files, the
 * closest to that quality), then it starts on the start worker and the
 * player opens, with the same serialization, guards and toasts ("Already
 * starting a video", "Something else is playing", ...) and
 * evo_twitch_launcher_starting() while it runs. What can't play says so:
 * "This video is for subscribers only", "This video is gone", "This clip is
 * gone", "No connection". A video picks up where it was left (30 s in or
 * more, and not in its last minute: twitch::resumeAt) with the toast
 * "Resumed at 1:23:45" (where the player really started it); Left past the
 * start goes back to the top. Neither
 * has chat, and the player draws them over the whole screen with a
 * timeline: Left/Right skip 10 s, holding them scrubs. R2's quality picker
 * works for both, and a switch carries on where it was. Main thread only. */
void evo_twitch_play_video(const char* id, const evo_twitch_video_info_t* info) EVO_TWITCH_NOEXCEPT;
void evo_twitch_play_clip(const char* slug, const evo_twitch_video_info_t* info) EVO_TWITCH_NOEXCEPT;

/* Called every frame from the main loop. When a stream URL is ready it
 * starts playback (on a worker) and switches to the player; on error it
 * shows a toast. Cheap when there is nothing to do. */
void evo_twitch_launcher_pump(void) EVO_TWITCH_NOEXCEPT;

/* The app is about to stop playback for good (the soft close, or shutdown
 * on SIGTERM): saves where the video playing is, and writes the watch
 * progress and the watch history now on this thread (evo_twitch_progress.h,
 * evo_twitch_history.h), as no worker may get to finish. Does nothing while
 * the start worker is inside the controller. Call before stopPlayback().
 * Main thread only. */
void evo_twitch_launcher_save_progress(void) EVO_TWITCH_NOEXCEPT;

/* 1 from a play request (or an autoplay that found its switch file) until the
 * stream has started or failed; then evo_twitch_play() works again. For the
 * screens' "Starting..." state. Covers busy() and the lookup before it, while
 * input still reaches the screens. Home's picture doesn't count. Main thread
 * only. */
int evo_twitch_launcher_starting(void) EVO_TWITCH_NOEXCEPT;

/* 1 while the start worker is inside PlaybackController::startPlaybackSource().
 * The controller has no locking, so while this is 1 the main thread must not
 * start or stop playback: Application holds the soft close's first stage,
 * keeps pad input from the screens (except while it is Home's picture that
 * starts, see evo_twitch_preview_starting()), and shutdown() waits. Main
 * thread only. */
int evo_twitch_launcher_busy(void) EVO_TWITCH_NOEXCEPT;

/* ---- What plays ---- */

/* The channel of a stream the launcher started, as Twitch described it when
 * the stream was looked up; for a video or clip, what its card said. Text
 * is UTF-8 as Twitch sent it. */
typedef struct {
    char login[32];        /* "" for a video or clip whose channel wasn't known */
    char name[96];         /* display name; the login when Twitch gave none */
    char title[512];       /* stream title (or the video's), "" when not known */
    char game[128];        /* category, "" when none or not known */
    int  viewers;          /* -1 when not known (always for a video or clip) */
    char avatar_url[512];  /* the 150 x 150 profile picture, "" when not known */
    /* When Twitch was asked, in monotonic milliseconds: the clock of
     * evo_twitch_data.h's data_now_ms() and its slots' stamps, so the newer
     * of this and the data service's now_playing() can win. */
    unsigned long long stamp;
    /* Stage 7: 0 for a live stream, else EVO_TWITCH_VIDEO_*. */
    int  video_type;
    char age[48];          /* "3 days ago", "" when not known */
    char curator[96];      /* a clip's maker, "" when not known */
    int  views;            /* a video's or clip's, -1 when not known */
} evo_twitch_now_playing_t;

/* The stream, video or clip playing now; NULL unless one the launcher
 * started is playing or paused (as evo_twitch_qualities()). A quality switch
 * keeps it. Points into the launcher's storage, good until the next one
 * starts. The player asks the data service for fresher details
 * (evo_twitch_data.h, now_playing()). Main thread only. */
const evo_twitch_now_playing_t* evo_twitch_now_playing(void) EVO_TWITCH_NOEXCEPT;

/* ---- Quality ----
 *
 * A stream starts in the default quality from Settings: "auto" is the best
 * the stream has; a quality it doesn't offer gives way to the closest lower
 * one, or the best when nothing is lower (twitch::pickQuality). In the player
 * R2 lists the stream's qualities and switches between them. */

/* One quality of the stream playing, as the quality picker lists it. */
#define EVO_TWITCH_QUALITY_MAX 12
typedef struct {
    char name[32];   /* Twitch's name: "chunked" (the source), "720p60", "audio_only" */
    char label[48];  /* what the picker shows: "1080p60 (source)", "720p60", "Audio only" */
    int  width;      /* 0 for audio only */
    int  height;
    int  fps;        /* rounded; 0 when Twitch doesn't say */
} evo_twitch_quality_t;

/* The qualities of the Twitch stream playing now, best first and audio only
 * last, as they were when the launcher started it. *list points into the
 * launcher's storage and stays good until the next stream starts. Returns how
 * many there are; 0 (and *list NULL) unless a stream the launcher started is
 * playing or paused (not while it starts, after it stopped or finished, or
 * while anything else plays). Main thread only. */
int evo_twitch_qualities(const evo_twitch_quality_t** list) EVO_TWITCH_NOEXCEPT;

/* The playing quality's index in evo_twitch_qualities(), -1 when there is no
 * list. Main thread only. */
int evo_twitch_current_quality(void) EVO_TWITCH_NOEXCEPT;

/* What a quality switch says for a past broadcast that is still being
 * recorded: it has no length yet, so it plays like a live stream and a
 * restart couldn't keep its place. The player's R2 says it too. */
#define EVO_TWITCH_QUALITY_LOCKED "Quality can't change while the broadcast is still on"

/* Switches the playing stream to the quality `name`, one of
 * evo_twitch_qualities(). Variant addresses are signed and short-lived, so a
 * worker looks the stream up again (token, master playlist, that quality, or
 * the closest one if it is gone) while the old quality keeps playing. Then,
 * on the main thread, the player goes back to the screen it was opened from,
 * and on the next frame the old quality stops and the new one starts on the
 * start worker like any other stream (the frame loop reads the player without
 * a lock); the player opens again once it has. Chat stays on the channel
 * throughout (evo_twitch_launcher_switching_channel). A video or clip starts
 * again where it was when it stopped (a video from the start of that
 * segment, so up to 10 s earlier). For a clip the qualities are its files
 * ("1080p60", "720p", ...), looked up again the same way. A stream paused
 * before the switch is paused again once the new quality has started. A
 * past broadcast still being recorded doesn't switch: it toasts
 * EVO_TWITCH_QUALITY_LOCKED.
 * Toasts "Switching to 720p60..." (with an ellipsis). A lookup that fails
 * toasts its reason the way a play request does ("No connection", ...), if
 * the stream still plays, and leaves the old quality playing. A lookup that
 * lands on the quality already playing toasts "720p60 isn't available now"
 * and only refreshes the list. A stream that stopped meanwhile, an exit
 * dialog the user opened, the app quitting or a play request that came in
 * drops the switch.
 * Returns 1 when the switch has started; 0 when `name` is already playing, or
 * when it is turned down: nothing of the launcher's is playing, `name` isn't
 * in the list, or a lookup or start is in flight (a toast says so). Main
 * thread only. */
int evo_twitch_switch_quality(const char* name) EVO_TWITCH_NOEXCEPT;

/* The login of the channel a quality switch is restarting, from the moment it
 * stops the old stream until the new one has started or failed; NULL
 * otherwise, and for a video or clip (they have no chat). Chat stays on that
 * channel instead of closing while nothing plays. Main thread only. */
const char* evo_twitch_launcher_switching_channel(void) EVO_TWITCH_NOEXCEPT;

#ifdef __cplusplus
}
#endif
