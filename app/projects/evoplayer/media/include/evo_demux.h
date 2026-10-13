/*
 * evo_demux.h — the demux thread: one av_read_frame loop feeding the video /
 * audio packet queues, plus the in-place (no-reopen) seek path it services.
 *
 * Verbatim move of the PROSPERO_TRUE_AV_SEEK region + the two PacketQueue
 * instances + the stream indices from main.c — Track A step A5 of
 * docs/modularisation-plan.md.
 *
 * TRANSITIONAL: still reads the playback-core decode context (play_fmt /
 * play_ctx / play_frame / play_pkt), the video-decode flags and g_pp_pb from
 * main.c as plain externs. The seek executor's codec-flush / clock-reset guts
 * move into the playback session at A7; the pp_playback notify calls become a
 * passed-in pointer.
 */
#ifndef EVO_DEMUX_H
#define EVO_DEMUX_H

#include <pthread.h>

#include "evo_packet_queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The demux thread's output queues. Cleared by start/stop_video_playback and
 * drained by the video-decode thread, all still in main.c. */
extern PacketQueue video_packet_queue;
extern PacketQueue audio_packet_queue;

/* Selected stream indices — written by start_video_playback's stream select. */
extern int video_stream_index;
extern int audio_stream_index;

/* Demux thread handle + run flag (spawned / joined by main.c). */
extern volatile int demux_thread_running;
extern pthread_t    demux_thread;

void *demux_thread_func(void *arg);

/* Queue an in-place seek to `target_seconds`; the demux thread performs it on
 * its next loop. `restore_paused` = leave playback paused afterwards.
 * `base_on_failure` is the position base (resume_base_offset_seconds) to put
 * back if the seek fails: a caller that set the base to the target before
 * asking passes the base it had, so playback reads where it really carries
 * on; a negative value leaves the base alone. Returns 0 when no file is open. */
int prospero_request_inplace_seek(double target_seconds, int restore_paused,
                                  double base_on_failure);

/* Forget a seek the demux thread never ran. PlaybackController's
 * stopPlayback() calls it once the demux thread has been joined, so a seek
 * asked of one stream can't run on, or hold the position of, the next. */
void prospero_seek_reset(void);

/* 1 from a seek request until the demux thread has repositioned the stream
 * and reset the clocks; 0 otherwise. Meanwhile the clocks still count from
 * where playback was, so the position is the target alone. Any thread. */
int prospero_seek_busy(void);

/*
 * Where the timeline starts in the container's own clock, in seconds: the
 * file's start_time when it is above 0, else 0. Every position the player
 * shows and every seek target is counted from here; the container's
 * timestamps (packets, frames, av_seek_frame) are not. Set by
 * PlaybackController after the probe, 0 while nothing is open.
 *
 * Most files start at 0, so this never mattered until Twitch's past
 * broadcasts: their HLS segments start at 62 s (checked live 2026-10-13),
 * so a seek to "1:00:00" landed a minute early and anything under a minute
 * was refused by FFmpeg 7.1's hls demuxer ("Decoder seek failed").
 */
extern double media_timeline_origin_seconds;

/* The container timestamp (in seconds) to hand av_seek_frame for a position
 * on the timeline: the origin added, plus a quarter second for an HLS
 * playlist, whose demuxer refuses a timestamp even a rounding error before
 * its first packet. */
double prospero_seek_container_seconds(double target_seconds);

#ifdef __cplusplus
}
#endif

#endif /* EVO_DEMUX_H */
