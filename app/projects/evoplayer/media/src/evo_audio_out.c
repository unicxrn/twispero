/*
 * evo_audio_out.c — media audio output path: decode → resample → AudioOut.
 *
 * Verbatim move of the audio-out ring / output thread, the audio sample mix,
 * the audio decode thread, and the AUDIO_TRACK_SWITCH region from main.c
 * (Track A step A3 of docs/modularisation-plan.md). No behaviour change: the
 * only edits are `static` → external linkage on the state main.c still touches
 * and the transitional extern block below for the playback-core / subtitle /
 * resume globals this code reads. All of that collapses into the evo_pb_*()
 * façade at A8.
 */
#include "evo_audio_out.h"

#include <math.h>
#include <string.h>
#include <unistd.h>

#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

#include "evo_packet_queue.h"
#include "evo_audio_resample.h"
#include "evo_adec.h"
#include "evo_av_clock.h"
#include "pp_playback.h"
#include "evo_boot_log.h"
#include "evo_playback.h"

#ifndef SCREEN_PLAYER
#define SCREEN_PLAYER 2
#endif

int sceAudioOutOutput(int handle, const void *ptr);

/* Home's preview: output silence, keep the clock moving. */
static int g_audio_silent = 0;

void evo_audio_set_silent(int on) { g_audio_silent = on ? 1 : 0; }

/* ---------------------------------------------------------------------------
 * TRANSITIONAL: playback-core and resume state still owned by main.c.
 * Replaced by evo_pb_*() accessors at A8.
 * ------------------------------------------------------------------------ */
extern int               screen;
extern pp_playback       g_pp_pb;
extern int               player_paused;
/* Start-of-stream pre-buffer hold - see the note in Bridge.cpp. */
extern volatile int      pb_prebuffer_hold;
extern volatile int      pb_scrub_hold;
extern double            video_clock_seconds;
extern double            first_video_pts_seconds;
extern int               video_stream_index;
extern int               video_decode_ready;
extern int               video_decode_done;
extern int               audio_stream_index;
extern int               evo_audio_channels;
extern AVFormatContext  *play_fmt;
extern AVCodecContext   *audio_ctx;
extern PacketQueue       audio_packet_queue;
extern double            resume_base_offset_seconds;
extern double            media_duration_sec;
extern double            requested_resume_seek_pos;
extern long long         controls_last_used_ms;
extern char              current_media_path[512];

#include "evo_toast.h"
long long now_ms(void);

/* ---------------------------------------------------------------------------
 * Audio-out session state (exported via evo_audio_out.h).
 * ------------------------------------------------------------------------ */
int audio_handle = -1;
static int16_t audio_accum[2048 * EVO_AUDIO_MAX_CH];
int audio_accum_pos = 0;

static int16_t audio_queue[AUDIO_QUEUE_BLOCKS][AUDIO_BLOCK_SAMPLES * EVO_AUDIO_MAX_CH];
/* The PTS of each queued block's first sample, in microseconds (evo_av_clock.h). */
static int64_t audio_queue_pts[AUDIO_QUEUE_BLOCKS];
volatile int audio_queue_read = 0;
volatile int audio_queue_write = 0;
volatile int audio_queue_count = 0;
volatile long long audio_samples_played = 0;
volatile long long audio_samples_decoded = 0;
volatile double audio_clock_seconds = 0.0;
volatile double audio_pts_seconds = 0.0;
double first_audio_pts_seconds = -1.0;
volatile double audio_seek_discard_until = -1.0;
volatile unsigned audio_blocks_dropped = 0;

int detected_audio_rate = 48000;
volatile int audio_thread_running = 0;
pthread_t audio_thread;

volatile int audio_decode_thread_running = 0;
pthread_t audio_decode_thread;

/* The decode side's block PTS (decode thread), the clock (output thread) and
 * the avsync line's timing (output thread). evo_audio_out_reset_clock() puts
 * them back while neither thread is running, or across a seek. */
static evo_apts_writer g_apts = { PROSPERO_AUDIO_OUTPUT_RATE, EVO_AV_NOPTS, 0, EVO_AV_NOPTS };
static evo_aclock g_aclock = { PROSPERO_AUDIO_OUTPUT_RATE, AUDIO_BLOCK_SAMPLES, 0, EVO_AV_NOPTS, 0,
                               EVO_AV_NOPTS, 0 };
static evo_avsync_log g_avsync_log = { -1, 0.0 };

void evo_audio_out_reset_clock(int new_stream)
{
    evo_apts_reset(&g_apts, PROSPERO_AUDIO_OUTPUT_RATE);
    evo_aclock_reset(&g_aclock, PROSPERO_AUDIO_OUTPUT_RATE, AUDIO_BLOCK_SAMPLES);
    evo_avsync_log_reset(&g_avsync_log);
    if (new_stream)
        audio_blocks_dropped = 0;
}

#ifdef EVO_USB_REMOTE
/* Dev remote `avdrop <ms>`: that much decoded audio is thrown away once, on
 * the decode thread, the way a skipped segment or a dropped block loses it. */
static volatile int g_avdrop_ms = 0;

void evo_audio_out_avdrop(int ms)
{
    if (ms > 0)
        g_avdrop_ms = ms;
}
#endif


static void audio_queue_push(int16_t *buf, int64_t pts_us) {
    int spins = 0;
    /* Wait for space instead of dropping — drops cause choppy audio on UHD */
    while (audio_queue_count >= AUDIO_QUEUE_BLOCKS &&
           audio_decode_thread_running && spins < 200) {
        usleep(500);
        spins++;
    }
    if (audio_queue_count >= AUDIO_QUEUE_BLOCKS) {
        /* Lost for good. The clock no longer minds (the next block brings its
         * own PTS), but the avsync line counts them. */
        audio_blocks_dropped++;
        return;
    }

    memcpy(audio_queue[audio_queue_write], buf,
           (size_t)AUDIO_BLOCK_SAMPLES * evo_audio_channels * sizeof(int16_t));
    audio_queue_pts[audio_queue_write] = pts_us;
    audio_queue_write = (audio_queue_write + 1) % AUDIO_QUEUE_BLOCKS;
    audio_queue_count++;
}

/*
 * The clock for the block just handed to the port: its PTS, relative to the
 * picture's anchor, so first_video_pts_seconds + audio_clock_seconds is the
 * PTS of the block playing - which is what evo_audio_master_media_us() hands
 * the video pacing. It used to be a count of samples played, which never
 * noticed audio that was lost on the way (#1).
 *
 * On a jump back to a new timeline the clock carries on and the anchor moves
 * instead; first_video_pts_seconds moves with it, so every relative clock
 * stays where it was and the video, whose timestamps jumped too, still lines
 * up with the audio.
 */
static double audio_clock_from_block(int64_t pts_us)
{
    const double anchor_s = first_video_pts_seconds;
    const int64_t anchor_us = anchor_s >= 0.0 ? (int64_t)llround(anchor_s * 1000000.0) : EVO_AV_NOPTS;
    int64_t shift_us = 0;
    const double clock = evo_aclock_play(&g_aclock, pts_us, anchor_us, &shift_us);
    if (shift_us != 0) {
        if (anchor_us != EVO_AV_NOPTS && first_video_pts_seconds >= 0.0)
            first_video_pts_seconds = (double)(anchor_us + shift_us) / 1000000.0;
        evo_boot_log("avsync: audio timestamps went back %.3f s, new anchor",
                     (double)-shift_us / 1000000.0);
    }
    return clock;
}

/*
 * The "avsync" line in evo.log: the PTS of the block now playing and of the
 * picture on screen, both relative to the anchor, how far apart they are (the
 * lip-sync offset, + is sound ahead) and how many decoded blocks were dropped.
 * Every 10 s, and at once when the offset moves more than 40 ms
 * (evo_av_clock.h). Nothing with no picture to compare (audio only).
 */
static void audio_note_avsync(void)
{
    const double anchor = first_video_pts_seconds;
    const int64_t shown_us = g_pp_pb.display_pts_us;
    if (!g_pp_pb.active || shown_us <= 0 || anchor < 0.0)
        return;
    const double a = audio_clock_seconds;
    const double v = (double)shown_us / 1000000.0 - anchor;
    const double off_ms = (a - v) * 1000.0;
    if (evo_avsync_log_due(&g_avsync_log, now_ms(), off_ms))
        evo_boot_log("avsync a=%.3f v=%.3f off=%+.0fms drops=%u", a, v, off_ms,
                     (unsigned)audio_blocks_dropped);
}

void *audio_output_thread(void *arg) {
    static int16_t silence[AUDIO_BLOCK_SAMPLES * EVO_AUDIO_MAX_CH];
    /*
     * State for the "video is not advancing" escape on the throttle below.
     * Video's own audio-master wait has the mirror of this (stuck_iters in
     * decode_next_video_frame); without it here, the two waits can hold each
     * other forever.
     */
    double video_rel_at_wait = -1.0;
    int    video_stuck_iters = 0;
    while (audio_thread_running) {
        /*
         * Hold output while a seek is still discarding video.
         *
         * The audio side is gated to the seek target, so it is ready to play
         * the instant the seek lands - but video still has to decode from the
         * keyframe to that target, which on a long-GOP 4K stream is nearly two
         * seconds. Letting audio run through that put it 0.9 s ahead by the
         * time the picture came back, which read as "video is late", tripped
         * the badly-late branch in decode_next_video_frame, and set it draining
         * queued packets every frame - so video could never catch up and
         * playback crawled at a few frames a second. Starting both at the
         * target together is the whole point of the gate; this is the other
         * half of it.
         */
        if ((g_pp_pb.active && g_pp_pb.seek_discarding) || pb_scrub_hold) {
            /* The clocks restart at the seek target; so does the stall
             * detector, so a pre-seek reading cannot leak past the seek.
             * pb_scrub_hold parks output for the same reason it parks the two
             * decode threads: while the seek bar is being dragged the picture
             * is frozen, so audio running on would only build a lead it has to
             * give back the moment the scrub commits. */
            video_rel_at_wait = -1.0;
            video_stuck_iters = 0;
            usleep(2000);
            continue;
        }
        if (evo_pb_presenting() && !player_paused && audio_handle >= 1) {
            /*
             * Compare like-for-like: audio_clock is the playing block's PTS
             * relative to the first video PTS, so video must be relative to
             * it too (not absolute PTS). Absolute compare freezes easy
             * 720p/YouTube when clocks diverge.
             */
            /*
             * No anchor yet means nothing has been presented since the last
             * open or seek, so there is no video position to hold audio back
             * against. Treating it as 0 disables the throttle below instead of
             * comparing the audio clock with a stale absolute PTS.
             */
            double video_rel = 0.0;
            if (g_pp_pb.active && g_pp_pb.display_pts_us > 0 && first_video_pts_seconds >= 0.0)
                video_rel = ((double)g_pp_pb.display_pts_us / 1000000.0) - first_video_pts_seconds;
            else if (first_video_pts_seconds >= 0.0)
                video_rel = video_clock_seconds - first_video_pts_seconds;
            if (video_rel < 0.0)
                video_rel = 0.0;

            if (video_stream_index >= 0 && first_video_pts_seconds < 0.0 &&
                video_decode_ready && !video_decode_done) {
                usleep(2000);
                continue;
            }

            /*
             * Hold audio back when it gets more than half a second ahead of
             * the picture - but only while the picture is actually moving.
             *
             * This throttle waits on video, and video's audio-master wait in
             * decode_next_video_frame() waits on audio. On hardware, after a
             * seek into 4K HEVC + E-AC-3, that pair latched (2026-09-28):
             * audio parked here, its decoded-PCM ring stayed full, the audio
             * decode thread stopped popping, the audio packet queue capped
             * out, the demux thread parked on it, the video packet queue
             * drained, video stopped decoding - so video_rel froze and this
             * test stayed true forever. video_frames=0 for two minutes.
             *
             * demux_wait_for_room() in evo_demux.c cuts that cycle at the
             * demux end; this cuts it here too. ~250 ms with no video progress
             * and audio plays regardless, exactly as video presents regardless
             * after ~120 ms with no audio progress.
             *
             * Any movement counts, in either direction: a seek re-anchors
             * first_video_pts_seconds, so video_rel drops back to ~0 and a
             * forward-only test would never see it move again - leaving the
             * throttle switched off for the rest of the file.
             */
            if (video_rel < video_rel_at_wait - 0.0005 ||
                video_rel > video_rel_at_wait + 0.0005) {
                video_rel_at_wait = video_rel;
                video_stuck_iters = 0;
            }
            if (video_rel >= EVO_AV_SYNC_SETTLE_SEC &&
                audio_clock_seconds > video_rel + EVO_AV_AUDIO_LEAD_MAX_SEC &&
                video_stuck_iters < 125) {
                video_stuck_iters++;
                usleep(2000);
                continue;
            }
            if (audio_queue_count > 0) {
                const int64_t block_pts = audio_queue_pts[audio_queue_read];
                const double anchor_s = first_video_pts_seconds;
                if (evo_aclock_stale(&g_aclock, block_pts,
                                     anchor_s >= 0.0 ? (int64_t)llround(anchor_s * 1000000.0)
                                                     : EVO_AV_NOPTS)) {
                    /* Decoded before the seek that just cleared the queue. */
                    evo_boot_log("avsync: skipped a block from before the seek (%.3f s, anchor %.3f s)",
                                 (double)block_pts / 1000000.0, anchor_s);
                    audio_queue_read = (audio_queue_read + 1) % AUDIO_QUEUE_BLOCKS;
                    audio_queue_count--;
                    continue;
                }
                sceAudioOutOutput(audio_handle,
                                  g_audio_silent ? silence : audio_queue[audio_queue_read]);
                audio_samples_played += AUDIO_BLOCK_SAMPLES;
                audio_clock_seconds = audio_clock_from_block(block_pts);
                audio_queue_read = (audio_queue_read + 1) % AUDIO_QUEUE_BLOCKS;
                audio_queue_count--;
                audio_note_avsync();
            } else {
                /*
                 * Soft underrun: silence without advancing media clock.
                 * Video wait loop must break if audio stays stuck (see decode).
                 */
                sceAudioOutOutput(audio_handle, silence);
            }
        } else {
            /* Reset throttle tracker while paused or inactive so resume starts clean */
            video_rel_at_wait = -1.0;
            video_stuck_iters = 0;
            usleep(200);
        }
    }
    return NULL;
}

static float audio_get_sample(AVFrame *af, int ch, int i) {
    int channels = af->ch_layout.nb_channels;
    if (channels <= 0) channels = 2;

    if (ch >= channels) ch = channels - 1;
    if (ch < 0) ch = 0;

    if (af->format == AV_SAMPLE_FMT_FLTP) {
        float *d = (float*)af->data[ch];
        return d ? d[i] : 0.0f;
    }

    if (af->format == AV_SAMPLE_FMT_FLT) {
        float *d = (float*)af->data[0];
        return d ? d[i * channels + ch] : 0.0f;
    }

    if (af->format == AV_SAMPLE_FMT_S16P) {
        int16_t *d = (int16_t*)af->data[ch];
        return d ? ((float)d[i] / 32768.0f) : 0.0f;
    }

    if (af->format == AV_SAMPLE_FMT_S16) {
        int16_t *d = (int16_t*)af->data[0];
        return d ? ((float)d[i * channels + ch] / 32768.0f) : 0.0f;
    }

    return 0.0f;
}

static int16_t audio_float_to_s16(float v) {
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    return (int16_t)(v * 30000.0f);
}

/* pts_us: the frame's PTS in microseconds, or EVO_AV_NOPTS. */
static void mix_audio_frame_to_queue(
    AVFrame *frame,
    int64_t pts_us
) {
    if (
        !frame ||
        frame->nb_samples <= 0
    ) {
        return;
    }

    if (
        !prospero_audio_resampler_configure(
            frame,
            evo_audio_channels,
            audio_ctx
        )
    ) {
        return;
    }

    int input_rate =
        frame->sample_rate > 0
            ? frame->sample_rate
            : prospero_audio_resampler_input_rate();

    int64_t delay =
        swr_get_delay(
            prospero_audio_resampler_ctx(),
            input_rate
        );

    int output_capacity =
        (int)av_rescale_rnd(
            delay + frame->nb_samples,
            PROSPERO_AUDIO_OUTPUT_RATE,
            input_rate,
            AV_ROUND_UP
        );

    if (output_capacity <= 0) {
        return;
    }

    /*
     * What comes out of swr_convert first is the input it still held from the
     * frames before (`delay` samples at the input rate), so the first output
     * sample is that much older than this frame. No PTS: the block PTS carries
     * on from the samples already counted.
     */
    evo_apts_frame(&g_apts,
                   pts_us == EVO_AV_NOPTS
                       ? EVO_AV_NOPTS
                       : pts_us - av_rescale(delay, 1000000, input_rate));

    /*
     * Grow-only scratch instead of av_samples_alloc/av_freep per packet. This
     * runs on every audio packet - about 43 a second for AAC - and the
     * capacity only ever changes when the stream layout does, so the malloc
     * and free were pure churn on the decode thread.
     */
    static uint8_t *output_buffer;
    static size_t   output_buffer_cap;

    size_t need = (size_t)output_capacity * (size_t)evo_audio_channels *
                  sizeof(int16_t);
    if (need > output_buffer_cap) {
        uint8_t *grown = (uint8_t *)av_realloc(output_buffer, need);
        if (!grown) {
            return;
        }
        output_buffer = grown;
        output_buffer_cap = need;
    }

    uint8_t *output_planes[1] = {
        output_buffer
    };

    int converted =
        swr_convert(
            prospero_audio_resampler_ctx(),
            output_planes,
            output_capacity,
            (const uint8_t * const *)
                frame->extended_data,
            frame->nb_samples
        );

    if (converted > 0) {
        const int16_t *samples = (const int16_t *)output_buffer;
        const int ch = evo_audio_channels;
        int index = 0;

#ifdef EVO_USB_REMOTE
        {
            static int avdrop_left = 0;   /* output samples still to throw away */
            static int avdrop_ms = 0;
            const int asked = g_avdrop_ms;
            if (asked > 0) {
                g_avdrop_ms = 0;
                avdrop_ms = asked;
                avdrop_left = (int)((long long)asked * PROSPERO_AUDIO_OUTPUT_RATE / 1000);
            }
            if (avdrop_left > 0) {
                int n = converted < avdrop_left ? converted : avdrop_left;
                evo_apts_skip(&g_apts, n);
                index += n;
                avdrop_left -= n;
                if (avdrop_left == 0)
                    evo_boot_log("avdrop: threw away %d ms of decoded audio", avdrop_ms);
            }
        }
#endif

        /*
         * Both buffers are interleaved with the same channel count, so this is
         * a straight copy - it just has to stop at each accumulator block
         * boundary to hand the block off. It used to run sample by sample and
         * channel by channel, re-reading the volatile thread flag on every
         * iteration, which also stopped the compiler vectorising it. Copy in
         * runs instead and check the flag once per run.
         */
        while (index < converted && audio_decode_thread_running) {
            int run = converted - index;
            const int space = AUDIO_BLOCK_SAMPLES - audio_accum_pos;
            if (run > space)
                run = space;

            memcpy(&audio_accum[(size_t)audio_accum_pos * ch],
                   &samples[(size_t)index * ch],
                   (size_t)run * (size_t)ch * sizeof(int16_t));

            evo_apts_write(&g_apts, audio_accum_pos, run);
            audio_accum_pos += run;
            audio_samples_decoded += run;
            index += run;

            if (audio_accum_pos >= AUDIO_BLOCK_SAMPLES) {
                while (
                    audio_decode_thread_running &&
                    audio_queue_count >=
                        AUDIO_QUEUE_BLOCKS - 2
                ) {
                    usleep(1000);
                }

                if (!audio_decode_thread_running) {
                    break;
                }

                audio_queue_push(audio_accum, evo_apts_block(&g_apts));
                audio_accum_pos = 0;
            }
        }
    }

}


/* The live native audio decoder, or NULL when this stream is on FFmpeg.
 * Owned by the playback controller; read only on the decode thread. */
evo_adec *g_adec = NULL;
/* One AAC frame is at most 2048 samples per channel; 64 KB covers stereo S16
 * with room to spare and matches the capacity the reference PoC uses. */
static int16_t g_adec_pcm[32 * 1024];

void *audio_decode_thread_func(void *arg) {
    AVFrame *af = av_frame_alloc();
    if (!af) return NULL;

    while (audio_decode_thread_running) {
        /*
         * pb_prebuffer_hold parks audio alongside video at open. It has to be
         * both or neither: audio is the master clock, so letting it run while
         * video is held would advance the clock against frames that have not
         * been decoded yet, and every one of them would arrive late.
         */
        if (player_paused || pb_prebuffer_hold || pb_scrub_hold || !evo_pb_presenting()) {
            /* Parked - see the note on the video decode thread. */
            usleep(5000);
            continue;
        }

        while (audio_decode_thread_running && audio_queue_count > 10) {
            usleep(1000);
        }

        AVPacket *pkt = packet_queue_pop(&audio_packet_queue);
        if (pkt && audio_seek_discard_until >= 0.0) {
            /*
             * Seek discard window. av_seek_frame lands on the keyframe at or
             * before the target, so everything up to the target is run-up the
             * viewer has already heard. The video side drops those frames in
             * pp_playback_push_frame; drop the matching audio here so both
             * clocks restart from the target together. Audio packets are
             * independently decodable and the decoder was just flushed, so the
             * first kept packet primes it exactly as a fresh open would.
             * A packet with no PTS clears the gate rather than being dropped.
             */
            double pkt_seconds = -1.0;
            if (pkt->pts != AV_NOPTS_VALUE && play_fmt && audio_stream_index >= 0)
                pkt_seconds = (double)pkt->pts *
                    av_q2d(play_fmt->streams[audio_stream_index]->time_base);

            /*
             * 25 ms of slack keeps the packet that straddles the target.
             * In MPEG-TS, PES packets carry PTS only every few packets; intermediate
             * packets have AV_NOPTS_VALUE. Discard them while the seek gate is active
             * rather than prematurely clearing the gate.
             * The cap is the safety net for a container whose audio PTS does
             * not share an origin with the seek target (a non-zero
             * start_time): rather than mute the track, give up on the gate
             * after a GOP's worth of packets and let everything through.
             */
            static double s_last_discard_target = -1.0;
            static int s_dropped = 0;
            if (audio_seek_discard_until != s_last_discard_target) {
                s_last_discard_target = audio_seek_discard_until;
                s_dropped = 0;
            }

            if (pkt_seconds >= 0.0) {
                if (pkt_seconds < audio_seek_discard_until - 0.025 &&
                    s_dropped < 2000) {
                    s_dropped++;
                    av_packet_free(&pkt);
                    continue;
                }
                s_dropped = 0;
                s_last_discard_target = -1.0;
                audio_seek_discard_until = -1.0;
            } else {
                if (s_dropped < 2000) {
                    s_dropped++;
                    av_packet_free(&pkt);
                    continue;
                }
                s_dropped = 0;
                s_last_discard_target = -1.0;
                audio_seek_discard_until = -1.0;
            }
        }
        if (!pkt) {
            /*
             * Do not terminate at EOF. The demux thread stays alive
             * so an in-place backward seek can refill this queue.
             */
            usleep(
                video_decode_done
                    ? 5000
                    : 1000
            );

            continue;
        }

        /*
         * Native decode (libSceAudiodec, AAC/MP3) produces interleaved S16 at
         * the stream rate. Wrap it in an AVFrame and hand it to the same
         * mix_audio_frame_to_queue() the FFmpeg path uses, so resampling to the
         * 48 kHz port, channel mapping and the accumulator are all unchanged.
         * g_adec is NULL for every other codec and after a native fatal, which
         * is what makes the fallback a one-line branch rather than a mode.
         */
        int handled_natively = 0;
        if (g_adec && evo_adec_active(g_adec) == EVO_ADEC_BACKEND_NATIVE) {
            int produced = evo_adec_decode(g_adec, pkt->data, pkt->size,
                                           g_adec_pcm, (int)sizeof(g_adec_pcm));
            if (produced < 0) {
                /* Latched fatal: drop to FFmpeg for the rest of the session. */
                evo_adec_close(g_adec);
                g_adec = NULL;
            } else {
                handled_natively = 1;
                int ch = evo_adec_channels(g_adec);
                int rate = evo_adec_rate(g_adec);
                if (produced > 0 && ch > 0 && rate > 0) {
                    av_frame_unref(af);
                    af->format = AV_SAMPLE_FMT_S16;
                    af->sample_rate = rate;
                    av_channel_layout_default(&af->ch_layout, ch);
                    af->nb_samples = produced / (int)sizeof(int16_t) / ch;
                    if (af->nb_samples > 0 &&
                        av_frame_get_buffer(af, 0) == 0) {
                        int64_t pts_us = EVO_AV_NOPTS;
                        memcpy(af->data[0], g_adec_pcm, (size_t)produced);
                        if (pkt->pts != AV_NOPTS_VALUE && play_fmt && audio_stream_index >= 0) {
                            audio_pts_seconds = pkt->pts *
                                av_q2d(play_fmt->streams[audio_stream_index]->time_base);
                            if (first_audio_pts_seconds < 0.0)
                                first_audio_pts_seconds = audio_pts_seconds;
                            pts_us = av_rescale_q(pkt->pts,
                                                  play_fmt->streams[audio_stream_index]->time_base,
                                                  AV_TIME_BASE_Q);
                        }
                        mix_audio_frame_to_queue(af, pts_us);
                    }
                    av_frame_unref(af);
                }
            }
        }

        if (!handled_natively && avcodec_send_packet(audio_ctx, pkt) == 0) {
            while (audio_decode_thread_running && avcodec_receive_frame(audio_ctx, af) == 0) {
                int64_t pts_us = EVO_AV_NOPTS;
                /* A frame's own PTS, else the decoder's guess from the DTS. */
                const int64_t pts = af->pts != AV_NOPTS_VALUE ? af->pts : af->best_effort_timestamp;
                if (pts != AV_NOPTS_VALUE && play_fmt && audio_stream_index >= 0) {
                    audio_pts_seconds = pts * av_q2d(play_fmt->streams[audio_stream_index]->time_base);
                    if (first_audio_pts_seconds < 0.0)
                        first_audio_pts_seconds = audio_pts_seconds;
                    pts_us = av_rescale_q(pts, play_fmt->streams[audio_stream_index]->time_base,
                                          AV_TIME_BASE_Q);
                }

                mix_audio_frame_to_queue(af, pts_us);
                av_frame_unref(af);
            }
        }

        av_packet_free(&pkt);
    }

    av_frame_free(&af);
    return NULL;
}
