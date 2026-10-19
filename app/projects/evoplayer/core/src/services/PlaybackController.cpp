#include "evo/services/PlaybackController.hpp"
#include "evo/interfaces/IMediaMetadataService.hpp"
#include "evo/Application.hpp"

#include "pp_playback.h"
#include "evo_playback.h"
#include "evo_error.h"
#include "evo_thread.h"
#include "evo_demux.h"
#include "evo_audio_out.h"
#include "evo_audio_resample.h"
#include "evo_packet_queue.h"
#include "evo_vdec.h"
#include "evo_direct_mem.h"   /* evo_mem_budget_log - the numbers behind the 1080p rule */

/*
 * malloc_shim.c (tools/native-app/stubs). Weak so the non-app-module compile
 * check, which never links the shim, still builds - there the calls no-op.
 */
extern "C" {
__attribute__((weak)) void evo_alloc_stats(uint64_t *live, uint64_t *peak, uint64_t *large_n);
__attribute__((weak)) void evo_alloc_map_info(uint64_t *fails, uint64_t *served_flex,
                                              uint64_t *served_anon, uint64_t *flex_avail);
__attribute__((weak)) void evo_alloc_direct_info(uint64_t *served, uint64_t *live,
                                                 uint64_t *peak);
}

/*
 * The heap as the malloc shim sees it. map_fail > 0 means an allocation was
 * refused outright - on 2026-09-25 that was the whole 4K AV1 failure, not
 * FFmpeg. direct_* is the shim's last-resort fallback (#94): nonzero means
 * flexible memory ran dry and direct memory carried the rest.
 */
#include "evo_boot_log.h"
/* Not static: Application's render loop also samples it during playback, so a
 * run that crashes still leaves the heap's last state in evo.log. */
extern "C" void evo_log_alloc_state(const char *when)
{
    uint64_t live = 0, peak = 0, large_n = 0;
    uint64_t fails = 0, flex = 0, anon = 0, avail = 0;
    uint64_t dserved = 0, dlive = 0, dpeak = 0;
    if (evo_alloc_stats)       evo_alloc_stats(&live, &peak, &large_n);
    if (evo_alloc_map_info)    evo_alloc_map_info(&fails, &flex, &anon, &avail);
    if (evo_alloc_direct_info) evo_alloc_direct_info(&dserved, &dlive, &dpeak);
    evo_boot_log("  alloc [%s] live=%lluMB peak=%lluMB large=%llu "
                 "map_fail=%llu flex=%llu anon=%llu flex_avail=%lluMB "
                 "direct=%llu direct_live=%lluMB direct_peak=%lluMB",
                 when,
                 (unsigned long long)(live >> 20),
                 (unsigned long long)(peak >> 20),
                 (unsigned long long)large_n,
                 (unsigned long long)fails,
                 (unsigned long long)flex,
                 (unsigned long long)anon,
                 (unsigned long long)(avail >> 20),
                 (unsigned long long)dserved,
                 (unsigned long long)(dlive >> 20),
                 (unsigned long long)(dpeak >> 20));
}

#include "evo_adec.h"
#include "evo_stream_io.h"
#include "pp_stage_breadcrumb.h"
#include "evo_toast.h"
#include "evo_boot_log.h"
#include "evo_boot_trace.h"
#include "prospero_thumbnail.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/mathematics.h>
#include <libswscale/swscale.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/time.h>

int sceAudioOutInit(void);
int sceAudioOutOpen(int userId, int type, int index, unsigned int len, unsigned int freq, unsigned int param);
int sceAudioOutClose(int handle);
}

#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <cstdlib>

// Global playback instance
extern pp_playback g_pp_pb;
extern AVFormatContext *play_fmt;
extern AVCodecContext *audio_ctx;
extern evo_vdec *g_vdec;
extern int g_vdec_force_ffmpeg;
extern AVPacket *video_pending_pkt;
extern struct SwsContext *play_sws;
extern int screen;
extern int player_paused;
extern double media_duration_sec;
extern double resume_base_offset_seconds;
extern double requested_resume_seek_pos;
extern char current_media_path[768];
extern int evo_audio_channels;
/* Start-of-stream pre-buffer hold - see the note in Bridge.cpp. */
extern volatile int pb_prebuffer_hold;
extern volatile int pb_scrub_hold;
extern int pb_rebuffer_enabled;
extern int video_packet_cap;
extern int audio_packet_cap;
extern long long video_queue_byte_cap;
extern long long audio_queue_byte_cap;

namespace evo {

/*
 * Ceiling on avformat_find_stream_info(). It has to clear the slowest probe we
 * expect to be worth waiting for - a long-GOP MPEG-TS over USB, where the 4 s
 * analyzeduration plus read time is the honest cost of finding a late audio
 * track - while still being short enough that a demuxer scanning a REMUX for a
 * Cluster it will not find gives up before the user does.
 */
static constexpr double kProbeDeadlineSeconds = 12.0;

/*
 * How far ahead of the decoder the demuxer reads on a network source.
 *
 * In seconds, because that is the only unit in which "enough cushion" means
 * the same thing for a 2 Mbit/s IPTV channel and a 54 Mbit/s UHD remux. The
 * byte ceilings are the memory guard: the flexible pool is the scarce one here
 * (~144 MB free during 4K playback, with map_fail already nonzero), so a
 * high-bitrate source gets fewer seconds rather than more megabytes.
 *
 * 10 s against the 4 s a flat 96-packet cap gave. This does not make a link
 * that cannot sustain the bitrate work - nothing can - but it rides out the
 * dips, and what it cannot ride out now ends in a clean rebuffer instead of
 * indefinite slow motion.
 */
static constexpr double    kReadAheadSeconds        = 10.0;
static constexpr long long kVideoReadAheadMaxBytes  = 48ll * 1024 * 1024;
static constexpr long long kVideoReadAheadMinBytes  =  8ll * 1024 * 1024;
static constexpr long long kAudioReadAheadMaxBytes  = 12ll * 1024 * 1024;

/* Leave headroom under PACKET_QUEUE_SIZE for demux_wait_for_room()'s bounded
 * overshoot, which doubles the cap when the other stream is starving. */
static constexpr int kVideoPacketCapMax = 384;
static constexpr int kAudioPacketCapMax = 512;

/*
 * With a packet data ring (evo_packet_queue.c) the queued packets live in one
 * block of direct memory instead of one mapping each in the flexible pool, so
 * the read-ahead can be deep: thirty seconds, which rides out the dips a
 * ten-second cushion cannot. TrueHD in Matroska runs at ~1200 packets/s, so
 * the audio slot count has to follow (it held 0.4 s at 512 and, because
 * av_read_frame() interleaves, held the video read-ahead to the same span).
 * The ring is allocated once and kept for the life of the process.
 */
static constexpr double    kRingReadAheadSeconds   = 30.0;
static constexpr size_t    kVideoRingBytes         = 512ull << 20;
static constexpr size_t    kAudioRingBytes         =  96ull << 20;
static constexpr long long kVideoRingMaxBytes      = 384ll << 20;
static constexpr long long kAudioRingMaxBytes      =  64ll << 20;
static constexpr int       kVideoPacketCapRing     = 8192;
static constexpr int       kAudioPacketCapRing     = 40000;

/*
 * Bits per second carried by the video stream, best effort.
 *
 * Matroska routinely reports neither a stream nor a container bitrate, and this
 * only sizes a buffer, so an unknown value falls back to the ceiling rather
 * than to a guess that could be an order of magnitude low.
 */
static long long videoBitsPerSecond(const AVFormatContext* fmt, int streamIndex)
{
    if (!fmt || streamIndex < 0 || streamIndex >= static_cast<int>(fmt->nb_streams))
        return 0;
    const AVStream* st = fmt->streams[streamIndex];
    if (st && st->codecpar && st->codecpar->bit_rate > 0)
        return static_cast<long long>(st->codecpar->bit_rate);
    /* Container total. It includes audio and subtitles, so it over-states the
     * video stream - which is the safe direction for a ceiling. */
    if (fmt->bit_rate > 0)
        return static_cast<long long>(fmt->bit_rate);
    return 0;
}

/*
 * Size the four demux ceilings for this source, and say so in evo.log: when a
 * stream stutters, the first question is whether the cushion was ever there.
 */
static void sizeReadAhead(bool networkSource, const AVFormatContext* fmt,
                          int videoIndex, double fps)
{
    if (!networkSource) {
        video_packet_cap      = 96;
        audio_packet_cap      = 96;
        video_queue_byte_cap  = 0;
        audio_queue_byte_cap  = 0;
        return;
    }

    const double safeFps = (fps > 1.0 && fps < 1000.0) ? fps : 30.0;

    /* Rings first: if direct memory refuses either, the old flexible-pool
     * budget below applies unchanged. */
    const bool ring = packet_queue_use_ring(&video_packet_queue, kVideoRingBytes) &&
                      packet_queue_use_ring(&audio_packet_queue, kAudioRingBytes);
    if (ring) {
        const long long vring = static_cast<long long>(packet_queue_ring_size(&video_packet_queue));
        const long long aring = static_cast<long long>(packet_queue_ring_size(&audio_packet_queue));
        /* A quarter of each ring stays free for the demuxer's bounded overshoot. */
        const long long vmax = std::min(kVideoRingMaxBytes, vring * 3 / 4);
        const long long amax = std::min(kAudioRingMaxBytes, aring * 3 / 4);

        int rvcap = static_cast<int>(kRingReadAheadSeconds * safeFps);
        if (rvcap < 96)                    rvcap = 96;
        if (rvcap > kVideoPacketCapRing)   rvcap = kVideoPacketCapRing;
        video_packet_cap = rvcap;

        const long long rvbps = videoBitsPerSecond(fmt, videoIndex);
        long long rvbytes = rvbps > 0
            ? static_cast<long long>(kRingReadAheadSeconds * static_cast<double>(rvbps) / 8.0)
            : vmax;
        if (rvbytes > vmax)                        rvbytes = vmax;
        if (rvbytes < kVideoReadAheadMinBytes)     rvbytes = kVideoReadAheadMinBytes;
        video_queue_byte_cap = rvbytes;

        audio_packet_cap     = kAudioPacketCapRing;
        audio_queue_byte_cap = amax;

        evo_boot_log("  pb: read-ahead %.0fs (ring) -> video %d pkt / %lld MB, "
                     "audio %d pkt / %lld MB (video %lld kbit/s @ %.2f fps)",
                     kRingReadAheadSeconds, video_packet_cap,
                     video_queue_byte_cap / (1024 * 1024),
                     audio_packet_cap, audio_queue_byte_cap / (1024 * 1024),
                     rvbps / 1000, safeFps);
        return;
    }
    evo_boot_log("  pb: packet ring unavailable - read-ahead stays in the flexible pool");

    int vcap = static_cast<int>(kReadAheadSeconds * safeFps);
    if (vcap < 96)                  vcap = 96;
    if (vcap > kVideoPacketCapMax)  vcap = kVideoPacketCapMax;
    video_packet_cap = vcap;

    const long long vbps = videoBitsPerSecond(fmt, videoIndex);
    long long vbytes = vbps > 0
        ? static_cast<long long>(kReadAheadSeconds * static_cast<double>(vbps) / 8.0)
        : kVideoReadAheadMaxBytes;
    if (vbytes > kVideoReadAheadMaxBytes) vbytes = kVideoReadAheadMaxBytes;
    if (vbytes < kVideoReadAheadMinBytes) vbytes = kVideoReadAheadMinBytes;
    video_queue_byte_cap = vbytes;

    /*
     * Audio has to read ahead as far as video does. av_read_frame() hands both
     * streams back in interleave order, so parking on a full audio queue stops
     * video filling too - a 96-packet audio cap put a hard 3 s ceiling on the
     * video read-ahead however deep video's own cap was. Packets per second
     * vary enormously by codec (31/s for 32 ms E-AC-3 frames against TrueHD's
     * far shorter ones), so give audio generous slots and let bytes bind.
     */
    audio_packet_cap     = kAudioPacketCapMax;
    audio_queue_byte_cap = kAudioReadAheadMaxBytes;

    evo_boot_log("  pb: read-ahead %.0fs -> video %d pkt / %lld MB, "
                 "audio %d pkt / %lld MB (video %lld kbit/s @ %.2f fps)",
                 kReadAheadSeconds, video_packet_cap,
                 video_queue_byte_cap / (1024 * 1024),
                 audio_packet_cap, audio_queue_byte_cap / (1024 * 1024),
                 vbps / 1000, safeFps);
}

#ifdef EVO_APP_MODULE
#  define SIO_STAGE(id, d) pp_stage_bc((id), (d))
#else
#  define SIO_STAGE(id, d) ((void)0)
#endif

/*
 * Pixel count of a video stream, or 0 if it is not one we could play.
 *
 * A stream whose codec parameters never resolved scores 0 rather than being
 * treated as a tiny one: an HLS master playlist can carry a variant that
 * find_stream_info could not pin down, and opening a decoder on it gives
 * width=height=0, which makes av_guess_frame_rate fall back to the 90 kHz
 * time base and leaves the pacer with a 0.01 ms frame budget.
 */
static long long videoStreamPixels(const AVStream* st) {
    if (!st || !st->codecpar) return 0;
    const AVCodecParameters* cp = st->codecpar;
    if (cp->codec_type != AVMEDIA_TYPE_VIDEO) return 0;
    if (cp->codec_id == AV_CODEC_ID_NONE)     return 0;
    if (cp->width <= 0 || cp->height <= 0)    return 0;
    /*
     * Cover art is a video stream with a width and a height, and album art is
     * routinely larger than the picture in a low-bitrate file. Scoring on
     * pixels would hand it the win and play a still image over the movie -
     * which "first video stream wins" happened not to do, since the art is
     * usually last. Attached pictures are never the thing to play.
     */
    if (st->disposition & AV_DISPOSITION_ATTACHED_PIC) return 0;
    return static_cast<long long>(cp->width) * cp->height;
}

/* Index into fmt->programs of the program carrying `stream_index`, or -1. */
static int programOfStream(const AVFormatContext* fmt, int stream_index) {
    if (!fmt || stream_index < 0) return -1;
    for (unsigned p = 0; p < fmt->nb_programs; ++p) {
        const AVProgram* prog = fmt->programs[p];
        if (!prog) continue;
        for (unsigned k = 0; k < prog->nb_stream_indexes; ++k) {
            if (prog->stream_index[k] == static_cast<unsigned>(stream_index))
                return static_cast<int>(p);
        }
    }
    return -1;
}

static bool streamInProgram(const AVFormatContext* fmt, int prog_idx, int stream_index) {
    if (!fmt || prog_idx < 0 || prog_idx >= static_cast<int>(fmt->nb_programs))
        return false;
    const AVProgram* prog = fmt->programs[prog_idx];
    if (!prog) return false;
    for (unsigned k = 0; k < prog->nb_stream_indexes; ++k) {
        if (prog->stream_index[k] == static_cast<unsigned>(stream_index))
            return true;
    }
    return false;
}

static uint64_t GetCurrentTimeMs() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<uint64_t>(tv.tv_sec) * 1000ULL + static_cast<uint64_t>(tv.tv_usec / 1000ULL);
}

PlaybackController::PlaybackController()
    : m_playbackFsm(PlaybackState::Stopped, "PlaybackFSM")
{
    initStateMachine();
}

PlaybackController::~PlaybackController() {
    stopPlayback();
}

void PlaybackController::initStateMachine() {
    m_playbackFsm
        .addState(PlaybackState::Stopped, "Stopped")
        .addState(PlaybackState::Opening, "Opening")
        .addState(PlaybackState::Playing, "Playing",
            []() {
                player_paused = 0;
                pp_playback_resume(&g_pp_pb);
            })
        .addState(PlaybackState::Paused, "Paused",
            []() {
                player_paused = 1;
                pp_playback_pause(&g_pp_pb);
            })
        .addState(PlaybackState::Scrubbing, "Scrubbing")
        .addState(PlaybackState::Seeking, "Seeking")
        .addState(PlaybackState::Finished, "Finished")
        .addState(PlaybackState::Error, "Error");

    // From Stopped
    m_playbackFsm
        .addTransition(PlaybackState::Stopped, PlaybackEvent::Open, PlaybackState::Opening)
        .addTransition(PlaybackState::Stopped, PlaybackEvent::Play, PlaybackState::Playing);

    // From Opening
    m_playbackFsm
        .addTransition(PlaybackState::Opening, PlaybackEvent::Play, PlaybackState::Playing)
        .addTransition(PlaybackState::Opening, PlaybackEvent::Fail, PlaybackState::Error)
        .addTransition(PlaybackState::Opening, PlaybackEvent::Stop, PlaybackState::Stopped);

    // From Playing
    m_playbackFsm
        .addTransition(PlaybackState::Playing, PlaybackEvent::Pause, PlaybackState::Paused)
        .addTransition(PlaybackState::Playing, PlaybackEvent::StartScrub, PlaybackState::Scrubbing)
        .addTransition(PlaybackState::Playing, PlaybackEvent::Seek, PlaybackState::Seeking)
        .addTransition(PlaybackState::Playing, PlaybackEvent::Finish, PlaybackState::Finished)
        .addTransition(PlaybackState::Playing, PlaybackEvent::Stop, PlaybackState::Stopped)
        .addTransition(PlaybackState::Playing, PlaybackEvent::Fail, PlaybackState::Error);

    // From Paused
    m_playbackFsm
        .addTransition(PlaybackState::Paused, PlaybackEvent::Resume, PlaybackState::Playing)
        .addTransition(PlaybackState::Paused, PlaybackEvent::Play, PlaybackState::Playing)
        .addTransition(PlaybackState::Paused, PlaybackEvent::StartScrub, PlaybackState::Scrubbing)
        .addTransition(PlaybackState::Paused, PlaybackEvent::Seek, PlaybackState::Seeking)
        .addTransition(PlaybackState::Paused, PlaybackEvent::Stop, PlaybackState::Stopped);

    // From Scrubbing
    m_playbackFsm
        .addTransition(PlaybackState::Scrubbing, PlaybackEvent::ConfirmScrub, PlaybackState::Seeking)
        .addTransition(PlaybackState::Scrubbing, PlaybackEvent::CancelScrub, PlaybackState::Playing)
        .addTransition(PlaybackState::Scrubbing, PlaybackEvent::Stop, PlaybackState::Stopped);

    // From Seeking
    m_playbackFsm
        .addTransition(PlaybackState::Seeking, PlaybackEvent::Play, PlaybackState::Playing)
        .addTransition(PlaybackState::Seeking, PlaybackEvent::Pause, PlaybackState::Paused)
        .addTransition(PlaybackState::Seeking, PlaybackEvent::Stop, PlaybackState::Stopped)
        .addTransition(PlaybackState::Seeking, PlaybackEvent::Fail, PlaybackState::Error);

    // From Finished / Error
    m_playbackFsm
        .addTransition(PlaybackState::Finished, PlaybackEvent::Open, PlaybackState::Opening)
        .addTransition(PlaybackState::Finished, PlaybackEvent::Stop, PlaybackState::Stopped)
        .addTransition(PlaybackState::Error, PlaybackEvent::Open, PlaybackState::Opening)
        .addTransition(PlaybackState::Error, PlaybackEvent::Stop, PlaybackState::Stopped);
}

bool PlaybackController::isActive() const {
    return evo_pb_is_active() || (video_stream_index >= 0 && video_thread_running) || m_musicMode;
}

/*
 * m_source is set from the moment a start begins until stopPlayback(), but
 * only some of that is playing. While Opening, a start may still be running
 * on a worker that writes m_source (the Twitch launcher starts on one; Play
 * is the last thing it posts). After
 * a failed open the source stays behind in Error. Neither counts, and in
 * neither is m_source read.
 */
const PlaybackSource* PlaybackController::currentSource() const {
    switch (m_playbackFsm.getCurrentState()) {
        case PlaybackState::Stopped:
        case PlaybackState::Opening:
        case PlaybackState::Error:
            return nullptr;
        default:
            return m_source.url.empty() ? nullptr : &m_source;
    }
}

bool PlaybackController::isPaused() const {
    return m_playbackFsm.getCurrentState() == PlaybackState::Paused;
}

/*
 * The same test PlayerScreen uses for its "loading" look: a frame of this
 * stream has been handed to the screen and no seek is throwing frames away
 * on the way to its target. video_frame_loaded is zeroed by stopPlayback(),
 * so it never speaks for the stream before.
 */
bool PlaybackController::isPictureReady() const {
    switch (m_playbackFsm.getCurrentState()) {
        case PlaybackState::Stopped:
        case PlaybackState::Opening:
        case PlaybackState::Error:
            return false;
        default:
            break;
    }
    if (m_musicMode || video_stream_index < 0) return true;
    return video_frame_loaded && !g_pp_pb.seek_discarding;
}

bool PlaybackController::isScrubbing() const {
    return m_playbackFsm.getCurrentState() == PlaybackState::Scrubbing;
}

double PlaybackController::getPositionSeconds() const {
    if (isScrubbing()) {
        return m_scrubTargetSeconds;
    }
    /* A seek on its way: the base is already the target, but the clocks still
     * count from where playback was until the demux thread resets them. Added
     * together they read past the target for a moment, which made the OSD
     * jump and put a wrong position into a video's saved progress. */
    if (prospero_seek_busy()) {
        const double base = resume_base_offset_seconds;
        return base < 0.0 ? 0.0 : base;
    }
    /* The global base, not m_resumeBaseOffset: the demux's in-place seek
     * moves it (and puts it back when it fails) without going through this
     * class. */
    double pos = resume_base_offset_seconds + evo_pb_position_s();
    return (pos < 0.0) ? 0.0 : pos;
}

double PlaybackController::getPercentage() const {
    if (m_durationSeconds <= 0.1) return 0.0;
    double pos = getPositionSeconds();
    double pct = pos / m_durationSeconds;
    if (pct < 0.0) pct = 0.0;
    if (pct > 1.0) pct = 1.0;
    return pct;
}

void PlaybackController::togglePause() {
    setPaused(!isPaused());
}

void PlaybackController::setPaused(bool paused) {
    if (paused) {
        m_playbackFsm.postEvent(PlaybackEvent::Pause);
    } else {
        m_playbackFsm.postEvent(PlaybackEvent::Resume);
    }
}

void PlaybackController::stopPlayback() {
    m_playbackFsm.postEvent(PlaybackEvent::Stop);
    player_paused = 0;
    resetScrubHold();

    if (demux_thread_running) {
        /* A network read can block for rw_timeout and then sit in FFmpeg's
         * reconnect loop; stop must not wait that out. */
        evo_stream_io_abort(m_streamIo);
        demux_thread_running = 0;
        pthread_join(demux_thread, nullptr);
    }
    /* With the demux thread gone nothing will run a seek still asked for:
     * forget it, or the next stream starts with it pending (its position
     * pinned to the old target, its first loop seeking there). */
    prospero_seek_reset();

    if (video_thread_running) {
        video_thread_running = 0;
        /*
         * Not a bare pthread_join: if the hardware decoder stops answering, the
         * decode thread never returns and a join here froze the whole app (#39).
         * The decoder watchdog flags that after its own limit; wait a little
         * longer than the longest call it tolerates, then abandon the thread.
         */
        for (int waitedMs = 0; !video_thread_exited && !evo_vdec_native_hung() && waitedMs < 25000;
             waitedMs += 10)
            usleep(10 * 1000);
        if (video_thread_exited) {
            pthread_join(video_thread, nullptr);
        } else {
            pthread_detach(video_thread);
            evo_bt("stopPlayback: video decode thread did not exit (decoder hung) - abandoned");
        }
    }

    if (audio_decode_thread_running) {
        audio_decode_thread_running = 0;
        pthread_join(audio_decode_thread, nullptr);
    }

    if (audio_thread_running) {
        audio_thread_running = 0;
        pthread_join(audio_thread, nullptr);
    }

    packet_queue_clear(&video_packet_queue);
    packet_queue_clear(&audio_packet_queue);

    audio_queue_count = 0;
    audio_queue_read = 0;
    audio_queue_write = 0;
    audio_accum_pos = 0;

    if (video_pending_pkt) {
        av_packet_free(&video_pending_pkt);
        video_pending_pkt = nullptr;
    }

    /*
     * The decode loop in evo_playback.c holds its half-consumed AU here. Left
     * behind, it is the first thing fed to the *next* file's decoder - a
     * mid-GOP packet from the previous stream.
     */
    if (video_video_pending_pkt) {
        av_packet_free(&video_video_pending_pkt);
        video_video_pending_pkt = nullptr;
    }

    /* Scaler is sized to the closed file's geometry; the next file may differ. */
    if (play_sws) {
        sws_freeContext(play_sws);
        play_sws = nullptr;
    }
    evo_playback_release_sw_scaler();

    if (audio_handle >= 1) {
        sceAudioOutClose(audio_handle);
        audio_handle = -1;
    }

    if (g_adec) {
        evo_adec_close(g_adec);
        g_adec = nullptr;
    }

    prospero_audio_resampler_destroy();

    if (audio_ctx) {
        avcodec_free_context(&audio_ctx);
        audio_ctx = nullptr;
    }

    if (g_vdec) {
        evo_vdec_close(g_vdec);
        g_vdec = nullptr;
    }

    if (play_fmt) {
        avformat_close_input(&play_fmt);
        play_fmt = nullptr;
    }

    /* Paired with evo_stream_io_open() in startPlaybackSource(). After
     * avformat_close_input, so nothing is still reading through it. */
    if (m_streamIo) {
        evo_stream_io_close(m_streamIo);
        m_streamIo = nullptr;
    }

    video_stream_index = -1;
    audio_stream_index = -1;
    m_durationSeconds = 0.0;
    media_duration_sec = 0.0;
    media_timeline_origin_seconds = 0.0;
    m_resumeBaseOffset = 0.0;
    resume_base_offset_seconds = 0.0;
    m_musicMode = false;
    m_currentFilePath.clear();
    m_source = PlaybackSource{};

    video_decode_ready = 0;
    video_decode_done = 0;
    video_frame_loaded = 0;

    /* Clear the pre-buffer hold on the way out. A file stopped mid-buffer
     * would otherwise leave it armed, and the next file's decode threads would
     * park on a hold that this file's demux thread is no longer running to
     * clear - a permanent freeze on a source that needs no buffering at all. */
    pb_prebuffer_hold = 0;
    pb_scrub_hold = 0;

    /*
     * Reset the media clocks. These are session globals, not per-file state:
     * carried over, the next file's frames are compared against the previous
     * file's audio clock, so `audio_clock_seconds > video_rel + 0.50` holds
     * forever in audio_output_thread() - the audio queue fills, the decode
     * thread blocks on it, the demuxer blocks on the decode thread, and the
     * second file never plays at all.
     */
    audio_samples_played = 0;
    audio_samples_decoded = 0;
    audio_clock_seconds = 0.0;
    audio_pts_seconds = 0.0;
    video_clock_seconds = 0.0;
    first_audio_pts_seconds = -1.0;
    first_video_pts_seconds = -1.0;
    audio_seek_discard_until = -1.0;
    evo_audio_out_reset_clock(1);

    /* Don't let the next file's scrub previews reuse this file's demuxer. */
    prospero_thumbnail_close_context();

    pp_playback_on_file_close(&g_pp_pb);
    pp_playback_log_stats(&g_pp_pb);
    evo_log_alloc_state("stop");
}

/*
 * #90: the local-file form. Everything a path can tell us is the path, so the
 * source carries only that and the title is derived from the filename
 * downstream, exactly as before.
 */
bool PlaybackController::startPlayback(const std::string& filePath, double resumeOffset) {
    PlaybackSource src;
    src.url = filePath;
    return startPlaybackSource(src, resumeOffset);
}

bool PlaybackController::startPlaybackSource(const PlaybackSource& source,
                                             double resumeOffset) {
    const std::string& filePath = source.url;
    if (filePath.empty()) {
        return false;
    }

    stopPlayback();

    m_playbackFsm.postEvent(PlaybackEvent::Open);
    m_source = source;
    m_currentFilePath = filePath;

    /*
     * current_media_path is read by the scrub preview and the demuxer, which
     * expect a filesystem path. A non-local source's URL (a Twitch stream) is
     * not one, and it carries a token, so it stays empty for one: those call
     * sites already treat an empty path as "nothing local".
     */
    if (source.isProvider())
        current_media_path[0] = 0;
    else
        std::snprintf(current_media_path, sizeof(current_media_path), "%s", filePath.c_str());

    /*
     * #90 scope 15: evo_stream_io_open() is the single open path for local and
     * network alike. It had no callers at all - every open went straight to
     * avformat_open_input - which meant the reconnect, reconnect_streamed and
     * timeout options it sets for a network URL existed and were never
     * applied. The timeout is the one that matters most: without it a stalled
     * HTTP open wedges the thread that called it, and on the app slot that is
     * the same class of hang as #39's decode call.
     *
     * For a local file it is the same avformat_open_input with the same
     * probesize plus the sequential read-ahead hint, so local playback is
     * unchanged.
     */
    evo_stream_io_config_t io_cfg;
    std::memset(&io_cfg, 0, sizeof io_cfg);
    io_cfg.ring_buffer_size = 8u * 1024u * 1024u;
    io_cfg.hls_from_start = source.from_start ? 1 : 0;

    int open_rc = evo_stream_io_open(filePath.c_str(), &play_fmt, &io_cfg, &m_streamIo);
    if (open_rc < 0) {
        m_playbackFsm.postEvent(PlaybackEvent::Fail);
        /* evo_stream_io_open recorded why (wrong link, refused login, dead
         * server, damaged file, a playlist where a stream belongs). */
        char why[256];
        const char* reason = evo_error_take(why, sizeof why) ? why : nullptr;
        if (source.quiet)
            evo_boot_log("open: quiet start failed: %s", reason ? reason : "no reason given");
        else
            toast_error(source.isProvider() ? "Couldn't open the stream" : "Couldn't open the file", reason);
        return false;
    }

    /*
     * avformat_find_stream_info() under a deadline, and not fatal if it runs
     * out.
     *
     * Two separate things were wrong here. The call had no bound, so a file
     * that sends the Matroska demuxer into matroska_resync() took the playback
     * thread with it - the 2026-09-28 log ends mid-scan, 32 s and 1.2 GB into
     * a DV P7 UHD REMUX, with no breadcrumb after P8_01e because there was
     * none to write. And the result was treated as pass/fail, when for MKV and
     * MP4 it is neither: the container header already carries codec id,
     * geometry and extradata, and find_stream_info only refines what is
     * mostly there. The poster path proves it on this exact file - it opened
     * the same REMUX, and decoded a 3840x2160 frame from it, seconds earlier.
     *
     * So: bound it, then judge the streams we actually ended up with rather
     * than the return code. A probe that timed out having already identified a
     * decodable video stream costs a frame-rate estimate, not the file.
     */
    SIO_STAGE("P8_02a_PRE_FIND_INFO", filePath.c_str());
    evo_stream_io_set_deadline(m_streamIo, kProbeDeadlineSeconds);
    const int find_rc = avformat_find_stream_info(play_fmt, nullptr);
    evo_stream_io_set_deadline(m_streamIo, 0.0);
    const bool probe_timed_out = evo_stream_io_deadline_expired(m_streamIo) != 0;
    {
        char d[64];
        std::snprintf(d, sizeof d, "rc=%d timeout=%d nb_streams=%u",
                      find_rc, probe_timed_out ? 1 : 0,
                      play_fmt ? play_fmt->nb_streams : 0u);
        SIO_STAGE("P8_02b_FIND_INFO_RC", d);
    }

    /*
     * The deadline is checked as well as the return code, not instead of it:
     * find_stream_info() swallows the AVERROR_EXIT from an interrupted read
     * and still returns the stream count it had managed to gather, so a probe
     * can time out and report success. Both cases leave the demuxer parked
     * wherever the scan stopped, and both need the rewind below.
     */
    /*
     * An open can succeed and still hold nothing. An HLS playlist whose variant
     * playlists the CDN refuses (HTTP 403, a signed link that is not ours, a
     * geo-block) opens with rc=0 - the master playlist itself was fine - and
     * nb_streams=0. Nothing then decodes, no frame ever arrives, and the player
     * sat on its buffering screen until the user backed out: hardware,
     * 2026-09-30, 110 s of video_frames=0 on a channel that could never play.
     * It is a failure like any other, so it takes the failure path below.
     */
    const bool no_streams = play_fmt && play_fmt->nb_streams == 0;

    if (find_rc < 0 || probe_timed_out || no_streams) {
        /*
         * Usable means: a stream we can hand to a decoder as-is. Geometry and
         * codec id come from the header, so this is satisfied for MKV/MP4 even
         * when the probe never ran to completion. An audio-only file counts -
         * music mode is a supported outcome below.
         */
        bool have_video = false;
        bool have_audio = false;
        for (unsigned int i = 0; play_fmt && i < play_fmt->nb_streams; ++i) {
            const AVStream* st = play_fmt->streams[i];
            if (!st || !st->codecpar) continue;
            const AVCodecParameters* cp = st->codecpar;
            if (cp->codec_type == AVMEDIA_TYPE_VIDEO &&
                cp->codec_id != AV_CODEC_ID_NONE &&
                cp->width > 0 && cp->height > 0) {
                have_video = true;
            } else if (cp->codec_type == AVMEDIA_TYPE_AUDIO &&
                       cp->codec_id != AV_CODEC_ID_NONE) {
                have_audio = true;
            }
        }

        if (!have_video && !have_audio) {
            evo_bt("PlaybackController: find_stream_info failed (rc=%d, timeout=%d) "
                   "and the header named no usable stream - giving up",
                   find_rc, probe_timed_out ? 1 : 0);
            avformat_close_input(&play_fmt);
            play_fmt = nullptr;
            evo_stream_io_close(m_streamIo);
            m_streamIo = nullptr;
            m_playbackFsm.postEvent(PlaybackEvent::Fail);
            if (!source.quiet)
                toast_error("Couldn't play this", probe_timed_out ? "Reading it took too long"
                                                  : no_streams    ? "The server sent nothing playable"
                                                                  : "No video or audio found in it");
            return false;
        }

        evo_bt("PlaybackController: find_stream_info incomplete (rc=%d, timeout=%d) - "
               "continuing on the container header (video=%d audio=%d)",
               find_rc, probe_timed_out ? 1 : 0, (int)have_video, (int)have_audio);

        /*
         * A probe that completes leaves its packets queued for av_read_frame to
         * replay, so the demuxer being parked past them does not matter. One
         * that is interrupted has no such guarantee: it was abandoned wherever
         * the scan had reached - 1.2 GB in, on the file this was written for -
         * and playback would start from there. Put it back on the first frame.
         * A resume offset seeks again below; this is the floor under it.
         */
        const int64_t rewind_to = (play_fmt->start_time != AV_NOPTS_VALUE)
                                      ? play_fmt->start_time : 0;
        const int seek_rc = av_seek_frame(play_fmt, -1, rewind_to, AVSEEK_FLAG_BACKWARD);
        if (seek_rc < 0) {
            evo_bt("PlaybackController: rewind after an incomplete probe failed (rc=%d)",
                   seek_rc);
        }

        if (probe_timed_out)
            toast_tech("Slow file", "Starting without a full scan");
    }

    /*
     * A live stream has no duration and cannot be seeked. Announce it rather
     * than leaving the OSD, the resume store and the #32-era seek/overlay path
     * to work it out from a seek that fails - none of them can.
     */
    if (m_source.isProvider() && !m_source.is_live && play_fmt->duration <= 0)
        m_source.is_live = true;

    if (play_fmt->duration > 0) {
        m_durationSeconds = static_cast<double>(play_fmt->duration) / static_cast<double>(AV_TIME_BASE);
        media_duration_sec = m_durationSeconds;
    }

    /* Where the timeline starts on the container's clock (evo_demux.h): a
     * Twitch past broadcast's segments start at about a minute. */
    media_timeline_origin_seconds =
        (play_fmt->start_time != AV_NOPTS_VALUE && play_fmt->start_time > 0)
            ? static_cast<double>(play_fmt->start_time) / static_cast<double>(AV_TIME_BASE)
            : 0.0;
    if (media_timeline_origin_seconds > 0.0)
        evo_boot_log("PlaybackController: the timeline starts at %.3fs on the container's clock",
                     media_timeline_origin_seconds);

    // Identify primary video and audio streams
    video_stream_index = -1;
    audio_stream_index = -1;

    /*
     * Pick the BEST video stream, not the first one.
     *
     * An HLS master playlist exposes every bitrate variant as its own streams,
     * and by convention lists the lowest first - so "first video stream wins"
     * played the worst rendition of every live channel. Hardware, 2026-09-28:
     * a 1080p channel opened at 320x180, a 720p one at 640x360, and a
     * 24-stream master whose first variant had no codec parameters at all
     * opened a decoder at 0x0 @ 90000 fps.
     *
     * Score on pixel count, tie-break on bitrate.
     */
    int chosen_program = -1;
    {
        long long best_pixels  = 0;
        int64_t   best_bitrate = -1;

        for (unsigned int i = 0; i < play_fmt->nb_streams; ++i) {
            const AVStream* st = play_fmt->streams[i];
            const long long pixels = videoStreamPixels(st);
            if (pixels <= 0) continue;

            const int64_t br = st->codecpar->bit_rate;
            if (pixels > best_pixels || (pixels == best_pixels && br > best_bitrate)) {
                best_pixels        = pixels;
                best_bitrate       = br;
                video_stream_index = static_cast<int>(i);
            }
        }

        /*
         * No video stream resolved its geometry. Fall back to the first one so
         * an awkward container still plays rather than being refused, and say
         * so - this is the shape that produced the 0x0 decoder.
         */
        if (video_stream_index < 0) {
            for (unsigned int i = 0; i < play_fmt->nb_streams; ++i) {
                const AVStream* st = play_fmt->streams[i];
                if (st && st->codecpar &&
                    st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                    video_stream_index = static_cast<int>(i);
                    evo_bt("PlaybackController: no video stream resolved its size - "
                           "falling back to stream %d", video_stream_index);
                    break;
                }
            }
        }

        chosen_program = programOfStream(play_fmt, video_stream_index);
    }

    /*
     * Audio from the SAME variant as the video where the demuxer groups them
     * into programs. Taking the best video from one rendition and audio from
     * another gives two different encodes of the same channel, which the
     * shared clock cannot reconcile. Decodable-first, exactly as below.
     */
    for (unsigned int i = 0; chosen_program >= 0 && i < play_fmt->nb_streams; ++i) {
        AVStream* st = play_fmt->streams[i];
        if (!st || !st->codecpar) continue;
        if (st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
        if (!streamInProgram(play_fmt, chosen_program, static_cast<int>(i))) continue;
        if (avcodec_find_decoder(st->codecpar->codec_id)) {
            audio_stream_index = static_cast<int>(i);
            break;
        }
    }

    for (unsigned int i = 0; i < play_fmt->nb_streams; ++i) {
        AVStream* st = play_fmt->streams[i];
        if (!st || !st->codecpar) continue;

        if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audio_stream_index < 0) {
            /*
             * Take the first audio stream we can actually decode, not simply
             * the first one.
             *
             * A Dolby Atmos title normally carries TrueHD first and an E-AC-3
             * track beside it for players that cannot manage TrueHD. Taking
             * stream order on trust meant picking the TrueHD, finding no
             * decoder, and playing nothing at all - not even the video, since
             * the clock waits on an audio stream that never opens - while a
             * decodable track sat one index away. Verified on hardware with
             * "Dolby Atmos TrueHD, E-AC-3 7.1.4.mkv": pos stayed at 0.00.
             */
            if (avcodec_find_decoder(st->codecpar->codec_id))
                audio_stream_index = static_cast<int>(i);
        }
    }

    /*
     * Nothing decodable: fall back to the first audio stream anyway, so the
     * file still opens and the track picker can show what is in it. Better a
     * silent file the user can inspect than a refusal with no explanation.
     */
    if (audio_stream_index < 0) {
        for (unsigned int i = 0; i < play_fmt->nb_streams; ++i) {
            AVStream* st = play_fmt->streams[i];
            if (st && st->codecpar &&
                st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                audio_stream_index = static_cast<int>(i);
                evo_bt("PlaybackController: no decodable audio stream (codec=%d) - "
                       "playing without sound",
                       (int)st->codecpar->codec_id);
                break;
            }
        }
    }

    /*
     * Stop the demuxer fetching the variants we did not choose.
     *
     * libavformat's hls demuxer downloads a variant's playlist whenever ANY of
     * its streams is still wanted, so leaving the others enabled pulls every
     * rendition of the channel at once - eight copies, on the 24-stream master
     * that prompted this. Whole programs go, not individual streams: one live
     * stream left in a variant keeps that whole variant downloading.
     *
     * Done last, so nothing we actually selected can be discarded.
     */
    if (chosen_program >= 0 && play_fmt->nb_programs > 1) {
        unsigned dropped = 0;
        for (unsigned p = 0; p < play_fmt->nb_programs; ++p) {
            if (static_cast<int>(p) == chosen_program) continue;
            AVProgram* prog = play_fmt->programs[p];
            if (!prog) continue;
            for (unsigned k = 0; k < prog->nb_stream_indexes; ++k) {
                const unsigned si = prog->stream_index[k];
                if (si >= play_fmt->nb_streams) continue;
                if (static_cast<int>(si) == video_stream_index ||
                    static_cast<int>(si) == audio_stream_index) continue;
                play_fmt->streams[si]->discard = AVDISCARD_ALL;
                ++dropped;
            }
        }
        evo_bt("PlaybackController: %u programs, using #%d - discarded %u streams "
               "from the other variants",
               play_fmt->nb_programs, chosen_program, dropped);
    }

    if (video_stream_index >= 0) {
        const AVCodecParameters* vcp = play_fmt->streams[video_stream_index]->codecpar;
        evo_bt("PlaybackController: video stream %d of %u (%dx%d, program %d)",
               video_stream_index, play_fmt->nb_streams,
               vcp ? vcp->width : 0, vcp ? vcp->height : 0, chosen_program);
    }

    m_musicMode = (video_stream_index < 0 && audio_stream_index >= 0);

    // Initialize Video Decoder if video present
    if (video_stream_index >= 0) {
        AVStream* vStream = play_fmt->streams[video_stream_index];

        AVRational fr = av_guess_frame_rate(play_fmt, vStream, nullptr);
        if (fr.num && fr.den)
            video_fps = (double)fr.num / (double)fr.den;
        else
            video_fps = 60.0;

        evo_vdec_open_params vp;
        std::memset(&vp, 0, sizeof(vp));
        /* No decoder setting: always Auto (native when the probe passed,
         * else FFmpeg). */
        vp.backend = g_vdec_force_ffmpeg
                         ? EVO_VDEC_BACKEND_FFMPEG
                         : evo_vdec_pref_resolve(EVO_VDEC_PREF_AUTO, vStream->codecpar->codec_id);
        vp.codec_id = vStream->codecpar->codec_id;
        vp.width = vStream->codecpar->width;
        vp.height = vStream->codecpar->height;
        vp.avctx_params = vStream->codecpar;
        /*
         * Software decode above 1080p - allowed by default since #95.
         *
         * This used to be refused, and the reason was always memory: first a
         * mis-measured "~180 MB" flexible pool, then - once the real 448 MB was
         * measured - the fear of an out-of-memory mid-playback, which on this
         * platform is a crash, not a stutter. #94 showed that fear was right:
         * 4K 10-bit dav1d drives flexible memory to 0, and the first refused
         * allocation broke FFmpeg's AV1 handling for the rest of the file.
         *
         * It is answered now. The malloc shim takes blocks of 1 MB and up from
         * direct memory first (115d9ff), EVO can hold at least 8 GB of it
         * (boot probe, 2026-09-26), and 4K 10-bit AV1 plays in real time and
         * seeks with flexible memory at 200+ MB free and map_fail=0.
         * docs/hardware/memory-budget.md has the figures.
         *
         * What is left is speed, not memory: a codec the CPU cannot decode in
         * real time at 4K plays slowly. Touch /mnt/usb0/evo_no_sw_4k to go back
         * to refusing - a field escape hatch, read once per session.
         *
         * This only concerns the software path: codecs the hardware cannot do
         * (AV1), or formats sceVideodec2 declines (HEVC 10-bit it will not take)
         * and hands back to FFmpeg - which is why the check also runs again on
         * the chosen backend after the open, below.
         */
        const bool sw_backend = (vp.backend != EVO_VDEC_BACKEND_NATIVE);
        const bool above_1080p = ((long)vp.width * (long)vp.height) > (1920L * 1080L);
        static int sw_4k_allowed = -1;
        if (sw_4k_allowed < 0)
            sw_4k_allowed = (access("/mnt/usb0/evo_no_sw_4k", F_OK) == 0) ? 0 : 1;

        if (sw_backend && above_1080p && sw_4k_allowed) {
            evo_boot_log("PlaybackController: %dx%d on the software decoder",
                         vp.width, vp.height);
            evo_mem_budget_log("sw-4k");
        }

        if (sw_backend && above_1080p && !sw_4k_allowed) {
            evo_boot_log("PlaybackController: refusing %dx%d on the software "
                         "decoder - /mnt/usb0/evo_no_sw_4k is set",
                         vp.width, vp.height);
            evo_mem_budget_log("sw-refuse");
            avformat_close_input(&play_fmt);
            play_fmt = nullptr;
            m_playbackFsm.postEvent(PlaybackEvent::Fail);
            toast_error("Can't play this", "4K software decode is switched off (evo_no_sw_4k)");
            return false;
        }
        vp.thread_count = 4;
        /* Frame threading for HEVC; slice threading for H.264 to prevent frame worker stack exhaustion */
        vp.thread_type = (vStream->codecpar->codec_id == AV_CODEC_ID_HEVC)
                            ? FF_THREAD_FRAME
                            : FF_THREAD_SLICE;
        vp.flag2_fast = (vStream->codecpar->codec_id == AV_CODEC_ID_HEVC);
        vp.skip_loop_filter = EVO_VDEC_KEEP;
        vp.skip_frame = EVO_VDEC_KEEP;
        vp.skip_idct = EVO_VDEC_KEEP;

        evo_vdec_backend chosen = EVO_VDEC_BACKEND_FFMPEG;
        g_vdec = evo_vdec_open(&vp, &chosen);
        /* The native decoder can decline a format it was asked for and hand
         * back FFmpeg - the same software-above-1080p case as above. */
        if (g_vdec && above_1080p && vp.backend == EVO_VDEC_BACKEND_NATIVE &&
            chosen != EVO_VDEC_BACKEND_NATIVE) {
            if (sw_4k_allowed) {
                evo_boot_log("PlaybackController: native declined %dx%d (codec=%d); "
                             "decoding it in software", vp.width, vp.height,
                             vStream->codecpar->codec_id);
                evo_mem_budget_log("native-declined-sw");
            }
        }
        if (g_vdec && above_1080p && chosen != EVO_VDEC_BACKEND_NATIVE && !sw_4k_allowed) {
            evo_boot_log("PlaybackController: native declined %dx%d (codec=%d); "
                         "refusing the FFmpeg fallback - /mnt/usb0/evo_no_sw_4k "
                         "is set", vp.width, vp.height,
                         vStream->codecpar->codec_id);
            evo_mem_budget_log("native-declined");
            evo_vdec_close(g_vdec);
            g_vdec = nullptr;
            avformat_close_input(&play_fmt);
            play_fmt = nullptr;
            m_playbackFsm.postEvent(PlaybackEvent::Fail);
            toast_error("Can't play this", "4K software decode is switched off (evo_no_sw_4k)");
            return false;
        }
        if (!g_vdec) {
            evo_boot_log("PlaybackController: failed to open video decoder");
        } else {
            evo_boot_log("PlaybackController: video decoder opened (backend=%s, codec=%d, %dx%d @ %.2f fps)",
                         (chosen == EVO_VDEC_BACKEND_NATIVE) ? "NATIVE (sceVideodec2)" : "FFmpeg",
                         vStream->codecpar->codec_id,
                         vp.width, vp.height, video_fps);
            /*
             * Allocator state at the moment decode starts. malloc_shim.c has
             * tracked this all along, but its only reader was main.c, so the
             * numbers went dark at the carve-up and three separate 4K crashes
             * were diagnosed by guesswork instead. Logged here because every
             * one of those crashes happened within ~100ms of this line:
             *   hevc10_pq_4k -> "get_buffer() failed"
             *   av1_4k       -> dav1d "Failed to read unit" / "parse temporal unit"
             * If fails>0 or flex_avail is small, it is the heap, not the codec.
             */
            evo_log_alloc_state("decode-open");
            /* The line above is the heap side only, which is why the direct
             * pool never entered the argument: nothing printed it. This is the
             * other half, at the same instant, so an accepted decode can be
             * compared against a refused one. */
            evo_mem_budget_log("decode-open");
        }
    }

    // Initialize Audio Decoder if audio present
    if (audio_stream_index >= 0) {
        AVStream* aStream = play_fmt->streams[audio_stream_index];
        const AVCodec* aDec = avcodec_find_decoder(aStream->codecpar->codec_id);
        if (aDec) {
            audio_ctx = avcodec_alloc_context3(aDec);
            if (audio_ctx && avcodec_parameters_to_context(audio_ctx, aStream->codecpar) >= 0) {
                /*
                 * Without pkt_timebase, libavcodec cannot rebase timestamps
                 * when it discards a codec's priming samples, and logs
                 * "Could not update timestamps for skipped samples". Opus
                 * always has pre-skip, so that path runs on its very first
                 * frame and essentially never for AAC/AC-3/E-AC-3 - which is
                 * exactly the pattern the codec sweep measured. The video and
                 * subtitle decoders have always set this; audio never did.
                 */
                audio_ctx->pkt_timebase = aStream->time_base;
                if (avcodec_open2(audio_ctx, aDec, nullptr) >= 0) {
                    sceAudioOutInit();

                    /*
                     * The grain passed to sceAudioOutOpen is how many frames
                     * sceAudioOutOutput() consumes per call, and the output
                     * thread hands it whole AUDIO_BLOCK_SAMPLES blocks. Open
                     * the port with any other grain and the tail of every
                     * block is silently discarded while audio_samples_played
                     * still advances by a full block - the audio clock then
                     * runs fast by exactly that ratio and drags the
                     * audio-mastered video pacing along with it.
                     */
                    int chCount = audio_ctx->ch_layout.nb_channels;
                    int handle = -1;

                    /* A surround source gets the 8-channel port; the
                     * resampler follows evo_audio_channels. */
                    if (chCount > 2) {
                        handle = sceAudioOutOpen(0xFF, 0, 0,
                                                 AUDIO_BLOCK_SAMPLES, 48000,
                                                 2 /* S16_8CH */);
                        if (handle >= 1) {
                            evo_audio_channels = 8;
                        } else if (!source.quiet) {
                            toast_tech("Audio", "Surround port refused, falling back to stereo");
                        }
                    }

                    /* Surround refused (or a genuinely stereo source): stereo. */
                    if (handle < 1) {
                        evo_audio_channels = 2;
                        handle = sceAudioOutOpen(0xFF, 0, 0,
                                                 AUDIO_BLOCK_SAMPLES, 48000,
                                                 1 /* S16_STEREO */);
                    }

                    audio_handle = handle;
                    evo_boot_log("audio: source %dch -> port %dch handle=%d",
                                 chCount, evo_audio_channels, handle);

                    if (audio_handle < 1 && !source.quiet) {
                        toast_error("No sound", "The PS5's audio output didn't start");
                    }

                    detected_audio_rate = (audio_ctx->sample_rate > 0)
                                              ? audio_ctx->sample_rate
                                              : 48000;

                    audio_queue_read = 0;
                    audio_queue_write = 0;
                    audio_queue_count = 0;
                    audio_accum_pos = 0;
                    evo_audio_out_reset_clock(1);

                    prospero_audio_resampler_reset();

                    /*
                     * Offload AAC/MP3 to libSceAudiodec when it will take the
                     * stream. audio_ctx stays open either way: it is what the
                     * OSD reads for codec metadata, and it is the fallback the
                     * decode thread drops to if a native AU ever fails.
                     */
                    evo_adec_open_params ap;
                    std::memset(&ap, 0, sizeof(ap));
                    ap.codec_id = aStream->codecpar->codec_id;
                    ap.sample_rate = aStream->codecpar->sample_rate;
                    ap.channels = chCount;
                    ap.extradata = aStream->codecpar->extradata;
                    ap.extradata_size = aStream->codecpar->extradata_size;

                    evo_adec_backend achosen = EVO_ADEC_BACKEND_FFMPEG;
                    g_adec = evo_adec_open(&ap, &achosen);
                    evo_boot_log("PlaybackController: audio %s (codec=%d, %dch, %d Hz)",
                                 (achosen == EVO_ADEC_BACKEND_NATIVE)
                                     ? "NATIVE (sceAudiodec)" : "FFmpeg",
                                 (int)ap.codec_id, ap.channels, ap.sample_rate);
                }
            }
        }
    }

    /*
     * Apply the resume position. This has to be a real seek on play_fmt before
     * the demux thread starts: m_resumeBaseOffset is added to the decode clock
     * for every position readout (OSD, progress bar), so setting it without
     * moving the file makes the whole UI report a time the picture is not at -
     * the OSD showed 5:15 while frame one was still 0:00. requested_resume_seek_pos
     * used to drive this in start_video_playback(); after the C++ migration it
     * had no reader at all.
     */
    /*
     * Seeded with the position we are about to seek to, not 0.
     *
     * Zeroing it here and only correcting it after the seek left a window -
     * an avformat open, probe and av_seek_frame on a 4K file, easily hundreds
     * of milliseconds - during which every position readout said 0:00. So
     * resuming a part-watched file visibly started at the beginning and then
     * snapped to the resume point.
     *
     * This does not break the invariant above. That rule is that the base must
     * agree with where the picture is; during the open there is no picture
     * yet, only the position it is about to arrive at. If the seek fails the
     * base goes back to 0 with it, which is where playback really will start.
     */
    const bool wantResume = (resumeOffset > 0.0 && resumeOffset < m_durationSeconds &&
                             (video_stream_index >= 0 || audio_stream_index >= 0));
    /* The discard target of a resume that took, on the container's clock
     * (as an in-place seek arms it); -1 for none. */
    int64_t resumeDiscardUs = -1;

    m_resumeBaseOffset = wantResume ? resumeOffset : 0.0;
    resume_base_offset_seconds = m_resumeBaseOffset;
    requested_resume_seek_pos = 0.0;

    if (wantResume) {
        double seekPos = resumeOffset;
        int seekStream = (video_stream_index >= 0) ? video_stream_index
                                                   : audio_stream_index;
        /* Land slightly early so the first decoded frame is at or before the
         * resume point rather than just past it. */
        if (video_stream_index >= 0 && seekPos > 3.0)
            seekPos -= 1.0;

        int64_t seekTs = static_cast<int64_t>(
            prospero_seek_container_seconds(seekPos) / av_q2d(play_fmt->streams[seekStream]->time_base));

        if (av_seek_frame(play_fmt, seekStream, seekTs, AVSEEK_FLAG_BACKWARD) >= 0) {
            evo_vdec_flush(g_vdec);
            if (g_adec)
                evo_adec_flush(g_adec);
            if (audio_ctx) {
                avcodec_flush_buffers(audio_ctx);
                prospero_audio_resampler_reset();
            }
            packet_queue_clear(&video_packet_queue);
            packet_queue_clear(&audio_packet_queue);

            m_resumeBaseOffset = resumeOffset;
            resume_base_offset_seconds = resumeOffset;
            /* The seek landed on a keyframe before resumeOffset. Drop the
             * run-up on both sides, as an in-place seek does (evo_demux.c):
             * the audio here, the video frames in pp_playback_push_frame()
             * once the markers below are armed. Both clocks then start at
             * resumeOffset, so the base is right from the first frame and the
             * position never jumps back by the keyframe gap. */
            if (resumeOffset > 0.05)
                audio_seek_discard_until = resumeOffset + media_timeline_origin_seconds;
            resumeDiscardUs = static_cast<int64_t>(
                (resumeOffset + media_timeline_origin_seconds) * 1000000.0);
        } else {
            /* Playback will start at 0, so the base has to follow it back. */
            m_resumeBaseOffset = 0.0;
            resume_base_offset_seconds = 0.0;
            evo_boot_log("PlaybackController: resume seek to %.2fs failed; starting at 0",
                         resumeOffset);
        }
    }

    // Load chapters
    if (auto metaService = Application::getInstance().getMediaMetadataService()) {
        metaService->loadChapters(play_fmt);
    }

    player_paused = 0;
    video_decode_ready = (video_stream_index >= 0) ? 1 : 0;
    video_decode_done = 0;
    evo_pb_reset_decode_fatal();
    pp_playback_on_file_open(&g_pp_pb);
    pp_playback_set_clock_source(&g_pp_pb, evo_audio_master_media_us, nullptr);
    /* After the open, which clears them: the resume's seek-begin and discard
     * markers (see the resume above). The seek itself is already done. */
    if (resumeDiscardUs >= 0) {
        pp_playback_notify_seek_begin(&g_pp_pb, resumeDiscardUs);
        pp_playback_notify_seek_end(&g_pp_pb, 1, 0, 0);
    }

    applyViewMode();

    /*
     * Pre-buffer a network source before letting the decoders run.
     *
     * Local files read far faster than real time, so their queue is full
     * before the first frame is wanted and holding would only add latency for
     * nothing. A network source is the one that arrives at roughly real time
     * with jitter on top, which is where the cushion earns its keep.
     *
     * Armed here and cleared by the demux thread; the decode threads created
     * below park on it themselves, so this call does not block - the frame
     * loop keeps running and PlayerScreen shows "BUFFERING..." meanwhile.
     */
    {
        const bool network_source =
            filePath.rfind("http://", 0) == 0 || filePath.rfind("https://", 0) == 0;
        sizeReadAhead(network_source, play_fmt, video_stream_index, video_fps);
        pb_prebuffer_hold   = network_source ? 1 : 0;
        pb_rebuffer_enabled = network_source ? 1 : 0;
        if (network_source)
            SIO_STAGE("P8_03_PREBUFFER_ARM", filePath.c_str());
    }

    // Start background worker threads
    demux_thread_running = 1;
    video_thread_running = (video_stream_index >= 0) ? 1 : 0;
    audio_decode_thread_running = (audio_ctx != nullptr) ? 1 : 0;
    audio_thread_running = (audio_handle >= 1) ? 1 : 0;

    evo_boot_log("  pb: threads demux=1 video=%d adec=%d aout=%d",
                 video_thread_running, audio_decode_thread_running,
                 audio_thread_running);
    evo_thread_create(&demux_thread, demux_thread_func, nullptr);

    if (video_thread_running) {
        video_thread_exited = 0;
        evo_thread_create(&video_thread, video_decode_thread_func, nullptr);
    }
    if (audio_decode_thread_running) {
        evo_thread_create(&audio_decode_thread, audio_decode_thread_func, nullptr);
    }
    if (audio_thread_running) {
        evo_thread_create(&audio_thread, audio_output_thread, nullptr);
    }

    m_playbackFsm.postEvent(PlaybackEvent::Play);
    return true;
}

double PlaybackController::clampScrubTarget(double target) const {
    if (target < 0.0) return 0.0;
    if (m_durationSeconds > 1.0 && target > m_durationSeconds) {
        return m_durationSeconds;
    }
    return target;
}

void PlaybackController::beginScrub() {
    if (!isActive()) return;

    /*
     * Capture the live position BEFORE entering Scrubbing.
     *
     * getPositionSeconds() opens with `if (isScrubbing()) return
     * m_scrubTargetSeconds;`, so once the FSM has taken the StartScrub
     * transition this line assigns the field to itself and the scrub head
     * never learns where playback actually is. It kept whatever the previous
     * scrub left behind - and 0.0 on the first scrub of a session, which is
     * why the bar and the clock jumped to the start of the file the moment the
     * D-pad was touched, then seeked to wherever the user nudged it from
     * there.
     */
    m_scrubTargetSeconds = getPositionSeconds();

    /*
     * Only Playing and Paused may start a scrub. The pause and the decode hold
     * below used to happen whether or not the FSM took the transition - and if
     * it refused, nothing was ever going to commit (tickScrubAutoCommit checks
     * isScrubbing()) or release them: the picture froze for good. Hardware,
     * 2026-10-01: D-pad seeks in streams started on a worker thread (EVO's
     * Emby and Jellyfin screens then) froze exactly so - no SEEK_BEGIN ever
     * logged. Settle the FSM to Playing (what the stream actually is) and try
     * once more; if it still refuses, hold nothing and let the caller seek
     * directly instead.
     */
    if (!m_playbackFsm.postEvent(PlaybackEvent::StartScrub)) {
        const int was = static_cast<int>(m_playbackFsm.getCurrentState());
        m_playbackFsm.postEvent(PlaybackEvent::Play);
        if (!m_playbackFsm.postEvent(PlaybackEvent::StartScrub)) {
            evo_boot_log("scrub: refused in playback state %d - seeking directly", was);
            return;
        }
        evo_boot_log("scrub: playback state %d settled to Playing first", was);
    }
    pp_playback_pause(&g_pp_pb);
    /* Park demux's two consumers for the drag. Everything decoded from here
     * until the commit would be thrown away by the clock pp_playback_pause()
     * just stopped; see pb_scrub_hold in Bridge.cpp. */
    pb_scrub_hold = 1;
    resetScrubHold();
}

void PlaybackController::moveScrub(double deltaSeconds) {
    if (!isScrubbing()) {
        beginScrub();
        if (!isScrubbing()) {
            /* No scrub to drag (see beginScrub): a plain relative seek beats
             * a frozen picture. */
            seekTo(clampScrubTarget(getPositionSeconds() + deltaSeconds));
            return;
        }
    }
    m_scrubTargetSeconds = clampScrubTarget(m_scrubTargetSeconds + deltaSeconds);
    m_scrubAutoCommitDeadlineMs = GetCurrentTimeMs() + 600ULL; // auto-commit after 600ms idle
}

bool PlaybackController::confirmScrub() {
    if (!isScrubbing()) return false;

    double target = m_scrubTargetSeconds;
    m_playbackFsm.postEvent(PlaybackEvent::ConfirmScrub);
    resetScrubHold();

    /* Nothing else clears this: the commit is the end of the drag, so the hold
     * has to come down here or the decode threads never restart. Released
     * before the seek rather than after, so the pipeline is already live when
     * the demux thread picks the request up - the couple of frames the decoder
     * may take from the old position in between are ones the seek discards
     * anyway. (Either order is safe for the demux handler's park wait: a
     * thread parked on any hold still sets video_decode_parked.) */
    pb_scrub_hold = 0;
    seekTo(target);

    m_playbackFsm.postEvent(PlaybackEvent::Play);
    return true;
}

void PlaybackController::cancelScrub() {
    if (!isScrubbing()) return;

    m_playbackFsm.postEvent(PlaybackEvent::CancelScrub);
    resetScrubHold();
    pb_scrub_hold = 0;
    pp_playback_resume(&g_pp_pb);
}

void PlaybackController::resetScrubHold() {
    m_scrubHoldDirection = 0;
    m_scrubHoldStartMs = 0;
    m_scrubHoldLastStepMs = 0;
    m_scrubAutoCommitDeadlineMs = 0;
}

void PlaybackController::updateScrubHold(uint32_t heldButtons) {
    if (!isScrubbing()) return;

    int dir = 0;
    if (heldButtons & PadButtons::Right) dir = 1;
    else if (heldButtons & PadButtons::Left) dir = -1;

    if (dir == 0) {
        m_scrubHoldDirection = 0;
        m_scrubHoldStartMs = 0;
        m_scrubHoldLastStepMs = 0;
        return;
    }

    uint64_t now = GetCurrentTimeMs();
    if (m_scrubHoldDirection != dir) {
        m_scrubHoldDirection = dir;
        m_scrubHoldStartMs = now;
        m_scrubHoldLastStepMs = now;
        return;
    }

    // Accelerate scrub hold rate
    uint64_t holdDuration = now - m_scrubHoldStartMs;
    uint64_t stepInterval = 100ULL; // step every 100ms
    if (now - m_scrubHoldLastStepMs >= stepInterval) {
        double speed = 1.0;
        if (holdDuration > 3000) speed = 30.0;
        else if (holdDuration > 1500) speed = 10.0;
        else if (holdDuration > 500) speed = 3.0;

        moveScrub(static_cast<double>(dir) * speed);
        m_scrubHoldLastStepMs = now;
    }
}

void PlaybackController::tickScrubAutoCommit() {
    if (isScrubbing() && m_scrubAutoCommitDeadlineMs > 0) {
        if (GetCurrentTimeMs() >= m_scrubAutoCommitDeadlineMs) {
            confirmScrub();
        }
    }
}

void PlaybackController::seekTo(double targetSeconds) {
    if (!isActive() || targetSeconds < 0.0) return;

    /* The base is the target from here on, so the position reads it while
     * the seek runs. If the seek fails, playback carries on where it was:
     * the demux thread (or the refusal below) puts this base back. */
    const double baseBefore = resume_base_offset_seconds;
    m_resumeBaseOffset = targetSeconds;
    resume_base_offset_seconds = targetSeconds;

    /* The frames carry the container's timestamps (evo_demux.h); the demux
     * thread arms the same target, so the two must agree. */
    int64_t targetUs = static_cast<int64_t>((targetSeconds + media_timeline_origin_seconds) * 1000000.0);
    if (!prospero_request_inplace_seek(targetSeconds, 0, baseBefore)) {
        /* Refused: nothing will run the seek, so nothing would ever restart the
         * clock a scrub paused. Carry on playing where we are instead. */
        evo_boot_log("seek: request to %.1fs refused - resuming", targetSeconds);
        m_resumeBaseOffset = baseBefore;
        resume_base_offset_seconds = baseBefore;
        pp_playback_resume(&g_pp_pb);
        return;
    }
    pp_playback_notify_seek_begin(&g_pp_pb, targetUs);
}

void PlaybackController::jumpChapter(int direction) {
    auto metaService = Application::getInstance().getMediaMetadataService();
    if (!metaService || metaService->getChapterCount() == 0) {
        // Default relative jump by 30 seconds
        double target = getPositionSeconds() + (direction > 0 ? 30.0 : -30.0);
        seekTo(clampScrubTarget(target));
        return;
    }

    int currentChapter = metaService->getChapterIndexAtPosition(getPositionSeconds());
    int targetChapter = currentChapter + direction;

    if (targetChapter >= 0 && targetChapter < static_cast<int>(metaService->getChapterCount())) {
        const ChapterInfo* ch = metaService->getChapter(targetChapter);
        if (ch) {
            seekTo(ch->startTimeSeconds);
            toast("Chapter", ch->title.c_str());
        }
    }
}

void PlaybackController::cycleViewMode() {
    int nextMode = (static_cast<int>(m_viewMode) + 1) % 3;
    setViewMode(static_cast<ViewMode>(nextMode));
}

void PlaybackController::setViewMode(ViewMode mode) {
    m_viewMode = mode;
    applyViewMode();
}

void PlaybackController::applyViewMode() {
    pp_aspect_mode aspect = PP_ASPECT_FIT;
    if (m_viewMode == ViewMode::Fill) aspect = PP_ASPECT_FILL;
    else if (m_viewMode == ViewMode::Stretch) aspect = PP_ASPECT_STRETCH;

    g_pp_pb.aspect = aspect;
    g_pp_pb.stats.aspect = static_cast<int>(aspect);
    pp_playback_set_output(&g_pp_pb, DisplayWidth, DisplayHeight, aspect);
}

} // namespace evo
