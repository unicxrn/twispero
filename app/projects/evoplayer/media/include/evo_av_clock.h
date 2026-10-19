/*
 * evo_av_clock.h - the audio master clock, kept on the audio's own timestamps.
 *
 * Video is paced against the audio (evo_audio_master_media_us). That clock
 * used to count the samples played since the first block, so audio that never
 * reached the speaker - a live HLS segment the demuxer skipped after a failed
 * fetch, a decoded block dropped on a full queue - left the sound ahead of the
 * picture by that much for the rest of the stream (#1). Now every PCM block
 * carries the PTS of its first sample, and playing a block sets the clock to
 * that PTS, the way ffplay and mpv do it.
 *
 * Three small pieces, all pure C with no FFmpeg or PS5 calls, so
 * tests/test_runner.c runs them on the host:
 *   evo_apts_*      the decode side: the PTS of each block as it is filled;
 *   evo_aclock_*    the output side: the clock, from the block being played;
 *   evo_avsync_log_* when to write the "avsync" line to evo.log.
 */
#ifndef EVO_AV_CLOCK_H
#define EVO_AV_CLOCK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* No timestamp. */
#define EVO_AV_NOPTS INT64_MIN

/*
 * Audio that steps back by more than this is on a new timeline (the stream
 * restarted, an ad was spliced in, the timestamps wrapped), not running late.
 * The clock carries on from where it was and the anchor moves instead, so the
 * relative clocks never go backwards. The video wait in decode_next_video_frame
 * uses the same figure: a picture this far ahead of the sound is on the other
 * side of such a jump, and waiting for the sound would only freeze it.
 */
#define EVO_AV_JUMP_US 1000000LL

/* ---------------------------------------------------------------- decode side
 * Blocks are filled from decoded frames (mix_audio_frame_to_queue). Call
 * evo_apts_frame() once per frame, before its samples, with the PTS of its
 * first output sample (after the resampler's delay), or EVO_AV_NOPTS to carry
 * on from the end of the frame before. Then evo_apts_write() for each run of
 * samples copied into the block, and evo_apts_block() once the block is full. */
typedef struct evo_apts_writer {
    int     rate;      /* output sample rate */
    int64_t base_us;   /* PTS of the write head `since` samples ago, or NOPTS */
    int64_t since;     /* samples written or skipped since base_us */
    int64_t block_us;  /* PTS of the first sample of the block being filled */
} evo_apts_writer;

void    evo_apts_reset(evo_apts_writer *w, int rate);
void    evo_apts_frame(evo_apts_writer *w, int64_t pts_us);
/* `n` samples go into a block that already held `fill`. */
void    evo_apts_write(evo_apts_writer *w, int fill, int n);
/* `n` samples are thrown away (the dev remote's avdrop). */
void    evo_apts_skip(evo_apts_writer *w, int n);
/* The PTS of the block being filled: its first sample's, or NOPTS. */
int64_t evo_apts_block(const evo_apts_writer *w);

/* ---------------------------------------------------------------- output side */
typedef struct evo_aclock {
    int     rate;
    int     block_samples;
    int     started;
    int64_t base_us;    /* PTS of the last block that had one */
    int64_t since;      /* samples played since base_us (blocks with no PTS) */
    int64_t own_anchor; /* the anchor while there is no picture: the first block */
    int     stale;      /* blocks thrown away since the reset (evo_aclock_stale) */
} evo_aclock;

void    evo_aclock_reset(evo_aclock *c, int rate, int block_samples);
/*
 * A block has been handed to the audio port. `pts_us` is its PTS, or NOPTS (it
 * then follows the block before, one block's length on). `anchor_us` is the
 * video's anchor, first_video_pts_seconds in microseconds, or NOPTS while no
 * picture has been shown (the first block is the anchor then).
 *
 * Returns the clock in seconds: the block's PTS minus the anchor, so that
 * anchor + clock is the PTS of the block playing. *shift_us is 0, except on a
 * jump back past EVO_AV_JUMP_US: then the anchor moves back by -*shift_us,
 * the clock carries on from the block before, and the caller moves
 * first_video_pts_seconds by the same amount.
 */
double  evo_aclock_play(evo_aclock *c, int64_t pts_us, int64_t anchor_us, int64_t *shift_us);
/* The PTS of the block playing now, or NOPTS before the first. */
int64_t evo_aclock_pts(const evo_aclock *c);
/*
 * Whether to throw a block away instead of playing it: only the first block
 * after a reset, and only when its PTS is more than EVO_AV_JUMP_US from the
 * anchor. The decode thread isn't held during a seek, so a block it was
 * filling can reach the queue after the seek cleared it; played first, it
 * would set the clock to the old position. Gives up after EVO_AV_STALE_MAX
 * blocks, so audio that really is that far from the picture still plays.
 */
#define EVO_AV_STALE_MAX 4
int     evo_aclock_stale(evo_aclock *c, int64_t pts_us, int64_t anchor_us);

/* ---------------------------------------------------------------- the log line
 * An "avsync" line goes to evo.log every EVO_AVSYNC_EVERY_MS while playing, and
 * at once when the offset has moved more than EVO_AVSYNC_STEP_MS from the last
 * line - but no closer than EVO_AVSYNC_GAP_MS to it, so a stall can't flood it. */
#define EVO_AVSYNC_EVERY_MS 10000
#define EVO_AVSYNC_STEP_MS  40.0
#define EVO_AVSYNC_GAP_MS   200

typedef struct evo_avsync_log {
    long long last_ms;    /* when the last line was written, -1 before the first */
    double    logged_ms;  /* the offset it showed */
} evo_avsync_log;

void evo_avsync_log_reset(evo_avsync_log *s);
/* 1 when a line is due now for this offset (and it is then taken as written). */
int  evo_avsync_log_due(evo_avsync_log *s, long long now_ms, double off_ms);

#ifdef __cplusplus
}
#endif

#endif /* EVO_AV_CLOCK_H */
