#include "evo/Application.hpp"
#include "evo/screens/PlayerScreen.hpp"
#include "evo/screens/SettingsScreen.hpp"
#include "evo/screens/AudioTrackPickerScreen.hpp"
#include "evo/screens/ModalDialogScreen.hpp"
#include "evo/screens/AboutSupportScreen.hpp"
#include "evo/screens/SafeToCloseScreen.hpp"
#include "evo/screens/TwitchHomeScreen.hpp"
#include "evo/screens/TwitchBrowseScreen.hpp"
#include "evo/screens/TwitchCategoryScreen.hpp"
#include "evo/screens/TwitchSearchScreen.hpp"
#include "evo/screens/TwitchChannelScreen.hpp"
#include "evo/screens/TwitchLoginScreen.hpp"
#include "evo/screens/TwitchFollowingScreen.hpp"
#include "evo/animation/AnimationManager.hpp"
#include "evo_about_text.hpp"

#include "evo_boot_log.h"
#include "evo_stream_io.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <sys/time.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "evo_agc_runtime.h"
#include "evo_toast.h"
#include <chrono>
extern "C" void evo_log_alloc_state(const char *when);  /* PlaybackController.cpp */

/* Defined in Bridge.cpp and read by every FPS readout in the app, but
 * nothing ever assigned it - so the player pill and the rail pill both
 * showed "0 FPS". The render loop is the only place that can measure it. */
extern "C" int perf_render_fps;
#include "evo_boot_trace.h"
#include "evo_jailbreak.h"
#include "evo_vdec.h"
#include "evo_direct_mem.h"
#include "evo_data_path.h"
#include "evo_toast.h"
#include "evo_theme.h"
#include "evo_keyboard.h"
#include "evo_feedback.h"
#include "evo_input.h"
#include "evo_net.h"
#include "evo_usb_remote.h"
#include "evo_twitch.h"
#include "evo_twitch_alerts.h"
#include "evo_twitch_auth.h"
#include "evo_twitch_chat.h"
#include "evo_twitch_chat_mode.h"
#include "evo_twitch_clip.h"
#include "evo_twitch_data.h"
#include "evo_twitch_images.h"
#include "evo_hui_twitch.h"
#include "pp_playback.h"
#include "evo_playback.h"
#include "evo_adec.h"
#include "evo_hw.h"
#include "evo_sysui.h"
#include "evo_rmlui_bridge.h"
#include "evo_perf_monitor.h"
#include "evo_vdec.h"

#include "evo_agc_runtime.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/cpu.h>
#include <libavutil/log.h>
#include <sys/stat.h>
#include <unistd.h>

int sceUserServiceInitialize(void *);
int sceUserServiceGetLoginUserIdList(int userId[4]);
int scePadInit(void);
int scePadOpen(int, int, int, void *);

typedef struct PS5_PadTouch {
    uint16_t x;
    uint16_t y;
    uint8_t id;
    uint8_t reserve[3];
} PS5_PadTouch;

typedef struct PS5_PadTouchData {
    uint8_t touchNum;
    uint8_t reserve[3];
    uint32_t time_since_touch_held_down;
    PS5_PadTouch touch[2];
} PS5_PadTouchData;

typedef struct PS5_PadData {
    uint32_t buttons;
    struct { uint8_t x; uint8_t y; } leftStick;
    struct { uint8_t x; uint8_t y; } rightStick;
    struct { uint8_t l2; uint8_t r2; uint8_t padding[2]; } analogButtons;
    float orientation[4];
    float acceleration[3];
    float angularVelocity[3];
    PS5_PadTouchData touchData;
    uint8_t rest[72];
} PS5_PadData;

static_assert(offsetof(PS5_PadData, touchData) == 52, "touchData offset mismatch");
static_assert(offsetof(PS5_PadData, touchData.touch[0].x) == 60, "touch[0].x offset mismatch");
static_assert(offsetof(PS5_PadData, touchData.touch[1].x) == 68, "touch[1].x offset mismatch");

int scePadReadState(int, PS5_PadData *);

extern pp_playback g_pp_pb;
extern int g_ps5_user_id;
extern int g_ps5_video_out_hdr;
extern int screen;
long long now_ms(void);
}

#ifndef EVO_PLAYER_VERSION
#define EVO_PLAYER_VERSION "v0.12.0"
#endif

#ifndef EVO_DIRECT_MEM_POOL_BYTES
#define EVO_DIRECT_MEM_POOL_BYTES (128 * 1024 * 1024)
#endif

extern "C" { volatile sig_atomic_t g_evo_term_requested = 0; }

#if defined(EVO_APP_MODULE) || defined(EVO_TARGET_PS5)
/* libSceSystemService, linked from the SDK stub - the link line already passes
 * --as-needed over the whole stub directory, so no PRX .syms entry is needed
 * (and a .syms line nothing imports would brick the module load). */
extern "C" int sceSystemServiceHideSplashScreen(void);
#endif

namespace evo {

static evo_input evo_pad_state;

/* The video's box was put beside the chat panel on the last player frame
 * that could read the source (see the player branch in run()). */
static bool g_chat_side_video = false;

/*
 * Route FFmpeg's logging into evo.log, and away from stderr.
 *
 * THIS IS A CRASH FIX, not a diagnostic nicety.
 *
 * av_log's default callback ends in fputs(str, stderr) - libavutil/log.c,
 * colored_fputs(). Nothing in this binary has ever written to stderr: the boot
 * log opens /mnt/usb0/evo.log itself and klog goes through
 * sceKernelDebugOutText, so on the native-app CRT stderr had never once been
 * touched, and touching it dereferences null. SIGSEGV, addr=0, no diagnostic.
 *
 * That is why crashes only ever happened on files that make FFmpeg *log*
 * something. A clean MP4 or MKV probes silently and hundreds of them worked
 * fine; an MPEG-TS reliably logs "start time for stream N is not set in
 * estimate_timings_from_pts" and died every single time, on three builds, with
 * that string still sitting in the register dump. The same dumps from
 * EVO_TEST_hevc8_4k.mp4 held "Error parsing NAL unit #0." - also an av_log
 * message. Each of those was read as a clue to where the code had got to; they
 * were the fault itself.
 *
 * Second prize: FFmpeg's own diagnostics finally land somewhere readable.
 * Several comments in this tree complain that libavcodec "writes to its own
 * av_log, which goes nowhere here" - it went to a null stderr.
 *
 * WARNING and above only, so a stream with per-frame complaints cannot flood
 * the log or slow the decode thread down.
 */
static void EvoAvLogCallback(void* avcl, int level, const char* fmt, va_list vl) {
    (void)avcl;
    if (level > AV_LOG_WARNING)
        return;
    char line[512];
    int n = vsnprintf(line, sizeof line, fmt, vl);
    if (n <= 0)
        return;
    if (n >= (int)sizeof line)
        n = (int)sizeof line - 1;
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
        line[--n] = '\0';
    if (n == 0)
        return;
    evo_bt("ffmpeg[%d]: %s", level, line);
}

static void evo_av_log_init() {
    av_log_set_callback(EvoAvLogCallback);
    av_log_set_level(AV_LOG_WARNING);
}

static void SoundEffectCallback(int sfxKind) {
    if (auto sfx = Application::getInstance().getSoundEffectEngine()) {
        sfx->playSound(static_cast<SoundEffect>(sfxKind));
    }
}

static void EnsureDataDirectories() {
    mkdir("/data", 0777);
    mkdir(evo_data_dir(), 0777);
}

/*
 * Deliberately never destroyed.
 *
 * A function-local static is destroyed at exit, and that is where QUIT EVO was
 * dying on 2026-09-18 - the breadcrumbs showed Application::shutdown() running
 * to "shutdown: complete", main() returning, and the process then faulting
 * while the static destructor chain unwound. ~Application() re-entered a
 * teardown that had already happened (its own shutdown() is guarded, but the
 * member unique_ptrs are not: ~PlaybackController ran stopPlayback() a second
 * time, which is the stats block that appears after "shutdown: complete"), and
 * the destruction order across these singletons is not something this process
 * needs to get right.
 *
 * By the time main() returns, shutdown() has already released everything that
 * actually matters - GPU, VideoOut, the direct-memory pool, decoders, audio -
 * in a verified order. What remains is C++ object destruction in a process
 * that is about to stop existing, so it buys nothing and costs a crash. The
 * heap "leak" is reclaimed with the process.
 *
 * This is the leaky-singleton form, not _exit(): main() still returns
 * normally, which the app module requires.
 */
Application& Application::getInstance() {
    static Application* s_instance = new Application();
    return *s_instance;
}

Application::Application()
    : m_appFsm(ApplicationState::Uninitialized, "ApplicationFSM")
{
    initStateMachine();
}

Application::~Application() {
    shutdown();
}

void Application::initStateMachine() {
    m_appFsm
        .addState(ApplicationState::Uninitialized, "Uninitialized")
        .addState(ApplicationState::InitializingHardware, "InitializingHardware")
        .addState(ApplicationState::InitializingServices, "InitializingServices")
        .addState(ApplicationState::InitializingScreens, "InitializingScreens")
        .addState(ApplicationState::Running, "Running")
        .addState(ApplicationState::Exiting, "Exiting")
        .addState(ApplicationState::Terminated, "Terminated");

    m_appFsm
        .addTransition(ApplicationState::Uninitialized, ApplicationEvent::Boot, ApplicationState::InitializingHardware)
        .addTransition(ApplicationState::InitializingHardware, ApplicationEvent::HardwareReady, ApplicationState::InitializingServices)
        .addTransition(ApplicationState::InitializingServices, ApplicationEvent::ServicesReady, ApplicationState::InitializingScreens)
        .addTransition(ApplicationState::InitializingScreens, ApplicationEvent::ScreensReady, ApplicationState::Running)
        .addTransition(ApplicationState::Running, ApplicationEvent::RequestExit, ApplicationState::Exiting)
        .addTransition(ApplicationState::Exiting, ApplicationEvent::ShutdownComplete, ApplicationState::Terminated)
        .addTransition(ApplicationState::Running, ApplicationEvent::ShutdownComplete, ApplicationState::Terminated);
}

void Application::requestExit() {
    m_running = false;
    m_appFsm.postEvent(ApplicationEvent::RequestExit);
}

/* See the note on the declaration in Application.hpp. */
void Application::requestSoftClose() {
    if (m_softClose)
        return;
    m_softClose = true;
    m_softCloseFrames = 0;
    evo_boot_log("soft close: requested");
    evo_boot_log_flush();
}

bool Application::initialize(int argc, char** argv) {
    (void)argc;
    (void)argv;

    evo_bt("Application::initialize entry");
    m_appFsm.postEvent(ApplicationEvent::Boot);
    evo_vdec_probe();
    /* Same pre-unjail constraint as the video decoder: after
     * evo_jailbreak_self() the credential swap breaks libSceAudiodec too. */
    evo_adec_native_probe();
    /* PS5 Pro detection, cached. The debug overlay names the console from
     * it. Runs before the unjail, like the decoder probes. */
    evo_hw_probe();
    /* The system's focus flag, for closes from the PS menu (evo_sysui.h).
     * Opened here, before the unjail, like the above. */
    evo_sysui_probe();

    if (!initHardware()) {
        return false;
    }
    m_appFsm.postEvent(ApplicationEvent::HardwareReady);

    if (!initServices()) {
        return false;
    }
    m_appFsm.postEvent(ApplicationEvent::ServicesReady);

    if (!initScreens()) {
        return false;
    }
    m_appFsm.postEvent(ApplicationEvent::ScreensReady);

    m_running = true;
    evo_bt("Application::initialize complete");
    return true;
}

bool Application::initHardware() {
#if defined(EVO_APP_MODULE)
    evo_bt("AGC: bare-metal AGC device context");
    int agcOk = evo_agc_runtime_init(DisplayWidth, DisplayHeight, 0);
    if (agcOk == 0) {
        /*
         * The runtime resolves what the panel is actually running at while it
         * brings VideoOut up, and may have settled on something other than the
         * 1080p default. Everything built below - the RmlUi contexts, the
         * video output rect, the UI scratch surface - has to use that size.
         */
        evo_agc_runtime_get_size(&DisplayWidth, &DisplayHeight);
        evo_bt("display: rendering at %dx%d", DisplayWidth, DisplayHeight);
        evo_boot_log_flush();
        evo_agc_runtime_frame_begin();
        evo_agc_runtime_present();
    }
    evo_keyboard_ime_probe();
    evo_boot_log_flush();
#endif

    evo_jailbreak_self();
    evo_boot_log_flush();

    evo_av_log_init();
    av_force_cpu_flags(0);
    evo_direct_mem_init(EVO_DIRECT_MEM_POOL_BYTES);
    /* The baseline, before anything has been decoded or drawn. Every later
     * figure is only meaningful against this one. */
    evo_mem_budget_log("boot");
    /* Follow-up to #94: how much of the ~11 GB of direct memory EVO can really
     * take, measured after the GPU runtime and resident decoders hold theirs.
     * The trigger file's content is the ceiling in MB (default 8192). */
    {
        FILE *pf = std::fopen("/mnt/usb0/evo_dm_probe", "r");
        if (pf) {
            long max_mb = 0;
            if (std::fscanf(pf, "%ld", &max_mb) != 1 || max_mb <= 0)
                max_mb = 8192;
            std::fclose(pf);
            evo_direct_mem_probe(static_cast<size_t>(256) << 20,
                                 static_cast<size_t>(max_mb) << 20);
            evo_boot_log_flush();
        }
    }
    EnsureDataDirectories();

    sceUserServiceInitialize(nullptr);
    scePadInit();

    int users[4] = {0};
    sceUserServiceGetLoginUserIdList(users);
    g_ps5_user_id = users[0];

    m_padHandle = scePadOpen(users[0], 0, 0, nullptr);
    if (m_padHandle < 0) {
        toast_error("Couldn't open the controller", nullptr);
    } else {
        toast_tech("Controller connected", nullptr);
    }

    evo_rmlui_init(DisplayWidth, DisplayHeight);
    /*
     * The three screen footers (list, browser, changelog) render m_version,
     * but only behind `if (!m_version.empty())` - and nothing in the app ever
     * called this, so m_version was always empty and all three fell through to
     * the literal baked into the .rml. They had been showing v0.7.1 ever since
     * that string was last hand-edited. Only the host tools set it, which is
     * why uiview looked right while the console did not.
     */
    const char* app_ver = (EVO_PLAYER_VERSION[0] == 'v' || EVO_PLAYER_VERSION[0] == 'V')
                          ? EVO_PLAYER_VERSION
                          : ("v" EVO_PLAYER_VERSION);
    evo_rmlui_set_version(app_ver);

    evo_input_reset(&evo_pad_state);
    evo_feedback_init(m_padHandle, SoundEffectCallback);

    evo_net_init();
    /*
     * avformat_network_init() was already here and was a no-op for the whole
     * 0.6-0.10 era, because the production FFmpeg profile was built
     * --disable-network. It does something now (#90 scope 1).
     */
    avformat_network_init();

    pp_playback_init(&g_pp_pb);
    pp_playback_set_output(&g_pp_pb, DisplayWidth, DisplayHeight, PP_ASPECT_FIT);
    evo_vdec_prefer_nv12(1);

    m_uiScratch = static_cast<uint32_t*>(calloc(static_cast<size_t>(DisplayWidth) * DisplayHeight, 4));
    return true;
}

bool Application::initServices() {
    m_settingsService = std::make_unique<SettingsService>();
    m_mediaMetadataService = std::make_unique<MediaMetadataService>();
    m_soundEffectEngine = std::make_unique<SoundEffectEngine>();
    m_playbackController = std::make_unique<PlaybackController>();
    m_screenManager = std::make_unique<ScreenManager>();

    m_soundEffectEngine->initialize();
    m_settingsService->loadSettings();

    toast(about::kAppName, (EVO_PLAYER_VERSION[0] == 'v' || EVO_PLAYER_VERSION[0] == 'V')
                         ? EVO_PLAYER_VERSION
                         : ("v" EVO_PLAYER_VERSION));
    return true;
}

bool Application::initScreens() {
    m_screenManager->registerScreen(std::make_unique<PlayerScreen>());
    m_screenManager->registerScreen(std::make_unique<SettingsScreen>());
    m_screenManager->registerScreen(std::make_unique<SettingsSectionScreen>(ScreenId::SettingsTwitch));
    m_screenManager->registerScreen(std::make_unique<SettingsSectionScreen>(ScreenId::SettingsControls));
    m_screenManager->registerScreen(std::make_unique<SettingsSectionScreen>(ScreenId::SettingsSystem));
    m_screenManager->registerScreen(std::make_unique<AudioTrackPickerScreen>());
    m_screenManager->registerScreen(std::make_unique<AboutSupportScreen>());
    m_screenManager->registerScreen(std::make_unique<SafeToCloseScreen>());
    m_screenManager->registerScreen(std::make_unique<ModalDialogScreen>());
    /* The Twitch rail. The launcher's autoplay remembers the screen it
     * started on (this one) and only plays while the user is still there. */
    m_screenManager->registerScreen(std::make_unique<TwitchHomeScreen>());
    m_screenManager->registerScreen(std::make_unique<TwitchFollowingScreen>());
    m_screenManager->registerScreen(std::make_unique<TwitchBrowseScreen>());
    m_screenManager->registerScreen(std::make_unique<TwitchSearchScreen>());
    /* Not on the rail themselves: other screens open them with show(). */
    m_screenManager->registerScreen(std::make_unique<TwitchCategoryScreen>());
    m_screenManager->registerScreen(std::make_unique<TwitchChannelScreen>());
    m_screenManager->registerScreen(std::make_unique<TwitchLoginScreen>());

    evo_twitch_selftest();
    m_screenManager->navigateTo(ScreenId::TwitchHome);
    evo_twitch::data_start();
    evo_twitch::images_start();
    evo_twitch::chat_start();
    evo_twitch::auth_start();
    evo_twitch_launcher_start();
    return true;
}

/*
 * Runs at most once, and tears down in dependency order.
 *
 * Two things were wrong here, both found by the first QUIT EVO on hardware
 * (2026-09-18) crashing after the pp_playback stats and before any AGC line:
 *
 * 1. It ran more than once. ~Application() calls it too, and Application is a
 *    singleton, so the destructor fires during static destruction after main()
 *    has returned - by which point run() had already torn everything down. The
 *    log showed the pp_playback stats block three times over. Nothing here was
 *    written to be re-entrant, so it is now guarded rather than every step
 *    being made individually idempotent.
 *
 * 2. It released the GPU before the UI that was using it. evo_rmlui_shutdown()
 *    existed but had no caller anywhere in the tree, so RmlUi kept its render
 *    interface, its compiled geometry and its textures while
 *    evo_agc_runtime_shutdown() closed VideoOut and unmapped the pool - and
 *    then unwound at static-destruction time against a runtime that was gone.
 *    The UI now goes first, while the runtime that owns its allocator is still
 *    alive to service the frees.
 *
 * Breadcrumbs at every step and flushed as they go: this path had no
 * instrumentation, so its first failure could only be located by what was
 * missing from the log.
 */
void Application::shutdown() {
    if (m_shutdownDone)
        return;
    m_shutdownDone = true;

    m_running = false;

    evo_boot_log("shutdown: begin");
    evo_boot_log_flush();

    /* A Twitch stream still opening on its worker: let it return (up to 40 s)
     * before stopPlayback() below - the controller has no locking. */
    if (evo_twitch_launcher_busy()) {
        evo_boot_log("shutdown: waiting up to 40 s for a Twitch stream start");
        evo_boot_log_flush();
        for (int i = 0; i < 400 && evo_twitch_launcher_busy(); ++i)
            usleep(100000);
        evo_boot_log("shutdown: Twitch stream start %s", evo_twitch_launcher_busy()
                     ? "still running after 40 s, going on" : "finished");
    }

    /* Where a Twitch video was, written now: nothing runs after this to
     * write it later (evo_twitch.h). Skipped while a start is still inside
     * the controller. */
    evo_twitch_launcher_save_progress();
    evo_boot_log_flush();

    if (m_playbackController) {
        m_playbackController->stopPlayback();
    }
    if (m_soundEffectEngine) {
        m_soundEffectEngine->shutdown();
    }
    evo_boot_log("shutdown: media stopped");
    evo_boot_log_flush();

    pp_playback_shutdown(&g_pp_pb);
    evo_boot_log("shutdown: pp_playback down");
    evo_boot_log_flush();

    /* Before the AGC runtime - see (2) above. */
    evo_rmlui_shutdown();
    evo_boot_log("shutdown: rmlui down");
    evo_boot_log_flush();

    evo_agc_runtime_shutdown();
    evo_boot_log("shutdown: agc runtime down");
    evo_boot_log_flush();

    if (m_uiScratch) {
        free(m_uiScratch);
        m_uiScratch = nullptr;
    }

    m_appFsm.postEvent(ApplicationEvent::ShutdownComplete);
    evo_boot_log("shutdown: complete");
    evo_boot_log_flush();
}

namespace {

/* The stick-click screenshot is for dev builds, the ones packaged with
 * --usb-remote (the dev remote's `key l3` reaches it too). A release build
 * takes no screenshots, so none of this is compiled into it. */
#if defined(EVO_USB_REMOTE)
#ifdef EVO_APP_MODULE
/*
 * A system notification, which is the only on-screen feedback available here.
 *
 * The app's own toast goes through a second Rml::Context and is disabled
 * (#75: it SIGSEGVs the plain .ffpfsc build on frame 1), so toast() draws
 * nothing and a screenshot gave no sign it had worked. This is the same
 * notification path --breadcrumbs uses for the boot trace; it is the
 * platform's, not RmlUi's, so it is unaffected.
 */
struct evo_sys_note { char pad[45]; char msg[3075]; };
extern "C" int sceKernelSendNotificationRequest(int, void *, unsigned long, int);

static void notifyOnScreen(const char* text) {
    evo_sys_note n;
    std::memset(&n, 0, sizeof n);
    std::snprintf(n.msg, sizeof n.msg, "%s", text);
    sceKernelSendNotificationRequest(0, &n, sizeof n, 0);
}
#else
static void notifyOnScreen(const char*) {}
#endif

/*
 * L3 screenshot.
 *
 * The feature survived only as a changelog line: nothing handled PadButtons::L3,
 * there was no BMP writer, and evo_agc_runtime_read_scanout() - which exists
 * precisely for this - had no callers. Captures the FRONT buffer (what is on
 * the panel right now), so it works on menus as well as during playback.
 *
 * 24-bit bottom-up BMP: the simplest format every viewer reads, and the same
 * one tools/shot.sh already expects.
 */
static bool write_scanout_bmp(FILE* fp);
#endif /* EVO_USB_REMOTE */

/* Leaves the file's anonymous namespace and evo for C linkage, then reopens
 * both. */
} // namespace
} // namespace evo

extern "C" void evo_remote_soft_close(void) {
    evo::Application::getInstance().requestSoftClose();
}

namespace evo {
namespace {

#if defined(EVO_USB_REMOTE)
bool evo_capture_screenshot(std::string& outPath) {
    /* Pick the next free slot so captures accumulate instead of overwriting. */
    char path[256];
    int slot = 0;
    for (; slot < 1000; ++slot) {
        std::snprintf(path, sizeof(path), "/mnt/usb0/evo_shot_%03d.bmp", slot);
        FILE* probe = std::fopen(path, "rb");
        if (!probe) break;
        std::fclose(probe);
    }
    if (slot >= 1000) return false;

    FILE* fp = std::fopen(path, "wb");
    if (!fp) {
        /* USB is the nice place for these (the user can pull them off without
         * FTP), but it is not always writable. Fall back to the data root
         * rather than just failing. */
        std::snprintf(path, sizeof(path), "%s/evo_shot_%03d.bmp", evo_data_dir(), slot);
        fp = std::fopen(path, "wb");
    }
    if (!fp) {
        evo_bt("screenshot: cannot open %s for writing", path);
        evo_boot_log_flush();
        return false;
    }
    if (!write_scanout_bmp(fp))
        return false;
    outPath = path;
    return true;
}

/* The front buffer as a 24-bit bottom-up BMP. Closes fp. */
static bool write_scanout_bmp(FILE* fp) {
    int w = 0, h = 0;
    evo_agc_runtime_get_size(&w, &h);
    if (w <= 0 || h <= 0) {
        evo_bt("screenshot: no render size (%dx%d)", w, h);
        evo_boot_log_flush();
        std::fclose(fp);
        return false;
    }

    std::vector<uint32_t> bgra(static_cast<size_t>(w) * static_cast<size_t>(h), 0u);
    evo_agc_runtime_read_scanout(bgra.data(), w, h);

    const int rowBytes = w * 3;
    const int pad = (4 - (rowBytes % 4)) % 4;
    const uint32_t pixelBytes = static_cast<uint32_t>((rowBytes + pad) * h);
    const uint32_t fileSize = 54u + pixelBytes;

    unsigned char hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = (unsigned char)(fileSize);       hdr[3] = (unsigned char)(fileSize >> 8);
    hdr[4] = (unsigned char)(fileSize >> 16); hdr[5] = (unsigned char)(fileSize >> 24);
    hdr[10] = 54;
    hdr[14] = 40;
    hdr[18] = (unsigned char)(w);        hdr[19] = (unsigned char)(w >> 8);
    hdr[20] = (unsigned char)(w >> 16);  hdr[21] = (unsigned char)(w >> 24);
    hdr[22] = (unsigned char)(h);        hdr[23] = (unsigned char)(h >> 8);
    hdr[24] = (unsigned char)(h >> 16);  hdr[25] = (unsigned char)(h >> 24);
    hdr[26] = 1;
    hdr[28] = 24;
    hdr[34] = (unsigned char)(pixelBytes);       hdr[35] = (unsigned char)(pixelBytes >> 8);
    hdr[36] = (unsigned char)(pixelBytes >> 16); hdr[37] = (unsigned char)(pixelBytes >> 24);
    std::fwrite(hdr, 1, sizeof(hdr), fp);

    std::vector<unsigned char> row(static_cast<size_t>(rowBytes + pad), 0u);
    for (int y = h - 1; y >= 0; --y) {           /* BMP rows run bottom-up */
        const uint32_t* src = bgra.data() + static_cast<size_t>(y) * static_cast<size_t>(w);
        for (int x = 0; x < w; ++x) {
            /*
             * Low byte is BLUE here, so it goes straight into the BMP's blue
             * slot.
             *
             * The UI composes colours as 0xAABBGGRR (MakeColorBgra), but the
             * scanout is registered COMP_SWAP=ALT - see the note on
             * EVO_AGC_LAYER_BYTES: "only the scanout uses COMP_SWAP=ALT for
             * BGRA" - so what lands in scanout memory is byte order B,G,R,A,
             * which read as a little-endian word is 0xAARRGGBB. I "corrected"
             * this to swap R and B and it was already right: every capture
             * after that came back with navy UI rendered brown.
             */
            const uint32_t px = src[x];
            row[x * 3 + 0] = (unsigned char)(px & 0xFF);           /* B */
            row[x * 3 + 1] = (unsigned char)((px >> 8) & 0xFF);    /* G */
            row[x * 3 + 2] = (unsigned char)((px >> 16) & 0xFF);   /* R */
        }
        std::fwrite(row.data(), 1, row.size(), fp);
    }
    std::fclose(fp);
    return true;
}
#endif /* EVO_USB_REMOTE */

/*
 * Playback frame-pacing trace - diagnostics only, no effect on playback.
 *
 * Every 5 s of playback it logs one "pace:" line: how many presents and new
 * video frames there were, how evenly the presents were spaced, what each
 * step of a player frame cost (video blit, UI, present incl. the flip wait),
 * and - the one that shows judder - for how many presents each video frame
 * stayed on screen. 59.94 fps on a 119.88 Hz output should be all "2"; a
 * "3" is a frame held one refresh longer than its neighbours.
 */
struct PaceTrace {
    using clk = std::chrono::steady_clock;
    clk::time_point window{}, last_present{};
    int presents = 0, frames = 0, since_frame = -1;
    int held[5] = {0};                     /* 1, 2, 3, 4, 5+ presents */
    int gap[4] = {0};                      /* <=12, 12-20, 20-30, >30 ms */
    double gap_sum = 0, gap_max = 0;
    double blit_sum = 0, blit_max = 0, ui_sum = 0, ui_max = 0, pres_sum = 0, pres_max = 0;
    int blits = 0, uis = 0;

    /* Where a whole loop iteration goes, phase by phase - so a stall outside
     * blit / ui / present can be pinned on its phase. */
    static constexpr int kPh = 8;
    clk::time_point ph_t{};
    double ph_cur[kPh] = {0}, ph_max[kPh] = {0};
    int slow_by[kPh] = {0};
    int iters = 0;

    void mark(int i) {
        const clk::time_point n = clk::now();
        if (ph_t != clk::time_point{}) ph_cur[i] += ms(ph_t, n);
        ph_t = n;
    }
    /* Loop top: close the previous iteration (the tail since present is
     * phase 7) and charge it to its slowest phase if it overran 30 ms. */
    void iter_begin() {
        mark(7);
        if (window == clk::time_point{}) {
            for (double& v : ph_cur) v = 0;
            return;
        }
        double total = 0;
        int worst = 0;
        for (int i = 0; i < kPh; ++i) {
            total += ph_cur[i];
            if (ph_cur[i] > ph_max[i]) ph_max[i] = ph_cur[i];
            if (ph_cur[i] > ph_cur[worst]) worst = i;
            ph_cur[i] = 0;
        }
        iters++;
        if (total > 30.0) slow_by[worst]++;
    }

    static double ms(clk::time_point a, clk::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    }
    static void add(double& sum, double& mx, double v) { sum += v; if (v > mx) mx = v; }

    void clear(clk::time_point now) {
        const clk::time_point lp = last_present, pt = ph_t;
        const int sf = since_frame;
        double cur[kPh];
        for (int i = 0; i < kPh; ++i) cur[i] = ph_cur[i];
        *this = PaceTrace();
        window = now;
        last_present = lp;
        since_frame = sf;
        ph_t = pt;
        for (int i = 0; i < kPh; ++i) ph_cur[i] = cur[i];
    }
    void stop() { *this = PaceTrace(); }
    void new_frame() {
        if (since_frame > 0) held[since_frame >= 5 ? 4 : since_frame - 1]++;
        since_frame = 0;
        frames++;
    }
    void blit(double d) { add(blit_sum, blit_max, d); blits++; }
    void ui(double d)   { add(ui_sum, ui_max, d); uis++; }
    void present(clk::time_point t0, clk::time_point t1) {
        if (window == clk::time_point{}) window = t0;
        presents++;
        if (since_frame >= 0) since_frame++;
        add(pres_sum, pres_max, ms(t0, t1));
        if (last_present != clk::time_point{}) {
            const double g = ms(last_present, t1);
            add(gap_sum, gap_max, g);
            gap[g <= 12.0 ? 0 : g <= 20.0 ? 1 : g <= 30.0 ? 2 : 3]++;
        }
        last_present = t1;
        const double span = ms(window, t1);
        if (span >= 5000.0 && presents > 1) {
            evo_boot_log("pace: %.1fs presents=%d (%.1f/s) video_frames=%d (%.2f/s) | gap avg %.2f max %.2f ms"
                         " [<=12:%d 12-20:%d 20-30:%d >30:%d] | blit avg %.2f max %.2f | ui avg %.2f max %.2f"
                         " | present avg %.2f max %.2f | held 1:%d 2:%d 3:%d 4:%d 5+:%d",
                         span / 1000.0, presents, presents * 1000.0 / span,
                         frames, frames * 1000.0 / span,
                         gap_sum / (presents - 1 > 0 ? presents - 1 : 1), gap_max,
                         gap[0], gap[1], gap[2], gap[3],
                         blits ? blit_sum / blits : 0.0, blit_max,
                         uis ? ui_sum / uis : 0.0, ui_max,
                         pres_sum / presents, pres_max,
                         held[0], held[1], held[2], held[3], held[4]);
            evo_boot_log("pace:   loop max ms pumps %.1f input %.1f update %.1f decide %.1f fetch %.1f"
                         " draw %.1f present %.1f tail %.1f | slow(>30ms) iters=%d by phase:"
                         " pumps %d input %d update %d decide %d fetch %d draw %d present %d tail %d",
                         ph_max[0], ph_max[1], ph_max[2], ph_max[3], ph_max[4], ph_max[5],
                         ph_max[6], ph_max[7],
                         slow_by[0] + slow_by[1] + slow_by[2] + slow_by[3] + slow_by[4] +
                         slow_by[5] + slow_by[6] + slow_by[7],
                         slow_by[0], slow_by[1], slow_by[2], slow_by[3], slow_by[4],
                         slow_by[5], slow_by[6], slow_by[7]);
            clear(t1);
        }
    }
};
PaceTrace g_pace;

} // namespace

int Application::run() {
    PS5_PadData padData;
    uint32_t lastButtons = 0;

    m_appFsm.postEvent(ApplicationEvent::StartLoop);
    evo_bt("Application: entering frame loop");
    evo_boot_log_flush();

    for (int frame = 0; m_running && !g_evo_term_requested; ++frame) {
        /*
         * Soft close, in three stages across successive iterations.
         *
         * The wait matters: the message has to be composited and presented
         * before submission stops, because what freezes on screen is whatever
         * the scanout last latched. Stop too early and the user is left
         * staring at a blank panel wondering whether it crashed.
         */
        if (m_softClosed) {
            usleep(200000);        /* parked: no input, no render, no present */
            continue;
        }
        /* Held while a Twitch stream is still opening on its worker, so the
         * stopPlayback() below never runs under it (evo_twitch.h). */
        if (m_softClose && !(m_softCloseFrames == 0 && evo_twitch_launcher_busy())) {
            if (m_softCloseFrames == 0) {
                /* Where a Twitch video was, before it stops (evo_twitch.h). */
                evo_twitch_launcher_save_progress();
                if (m_playbackController)  m_playbackController->stopPlayback();
                if (m_soundEffectEngine)   m_soundEffectEngine->shutdown();
                pp_playback_shutdown(&g_pp_pb);
                evo_boot_log("soft close: media released");

                evo_boot_log_flush();
                /* The safe-to-close screen, not a toast: it is what the
                 * parked loop leaves latched on the panel, so it has to say
                 * everything on its own. */
                if (m_screenManager)
                    m_screenManager->navigateTo(ScreenId::SafeToClose);
            }
            if (++m_softCloseFrames > 150) {     /* 150 presents */
                evo_agc_runtime_wait_idle(500);
                m_softClosed = true;
                evo_boot_log("soft close: parked - GPU idle, display latched, "
                             "safe to close from the switcher");
                evo_boot_log_flush();
                evo_usb_remote_mark_parked();
                continue;
            }
        }

        g_pace.iter_begin();
        if (frame < 5) {
            evo_bt("frame %d: start poll", frame);
            evo_boot_log_flush();
        }

        /* Render FPS over a ~500 ms window: long enough to be steady, short
         * enough to react. Sampled here rather than per-screen so the menus and
         * the player report the same number. */
        {
            static struct timespec s_fps_mark = {0, 0};
            static int s_fps_frames = 0;
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (s_fps_mark.tv_sec == 0 && s_fps_mark.tv_nsec == 0) {
                s_fps_mark = now;
            } else {
                ++s_fps_frames;
                const double elapsed =
                    static_cast<double>(now.tv_sec - s_fps_mark.tv_sec) +
                    static_cast<double>(now.tv_nsec - s_fps_mark.tv_nsec) / 1e9;
                if (elapsed >= 0.5) {
                    perf_render_fps =
                        static_cast<int>(static_cast<double>(s_fps_frames) / elapsed + 0.5);
                    s_fps_frames = 0;
                    s_fps_mark = now;
                }
            }
        }

        evo_net_poll();
        evo_usb_remote_poll();
        evo_twitch_launcher_pump();
        evo_twitch::data_frame();
        /* Between the two: the followed live slot and the login it is for
         * belong together until auth_frame() moves the login on. */
        evo_twitch::live_alerts_frame();
        evo_twitch::auth_frame();
        {
            /* "Your Twitch login ran out. Log in again.", once, wherever
             * the user is. */
            std::string notice, detail;
            evo_twitch::ClipResult clip;
            auto clipWaitsForPlayer = [this] {
                const ScreenId id = m_screenManager ? m_screenManager->getCurrentScreenId() : ScreenId::TwitchHome;
                return id == ScreenId::Player || id == ScreenId::ExitConfirm || id == ScreenId::AudioTrackPicker;
            };
            if (evo_twitch::auth_take_notice(&notice)) toast_error(notice.c_str(), nullptr);
            /* A chat message that didn't go out ("Message not sent: ..."),
             * also when the player was left after sending it. One at a time,
             * once the toast before it is gone, so none is lost under
             * another. */
            else if (!evo_toast_visible() && evo_twitch::chat_take_send_notice(&notice))
                toast_error("Chat", notice.c_str());
            /* A clip made with Square whose outcome came after the player
             * was left: its address, or why there is none. The player takes
             * its own (the card), and the exit dialog and the quality picker
             * over it leave the outcome for the player to come back to. */
            else if (!evo_toast_visible() && !clipWaitsForPlayer() && evo_twitch::clip_take_result(&clip)) {
                if (clip.made) {
                    notice = std::string(evo_twitch::kClipPageShort) + clip.id;
                    toast_success("Clip made", notice.c_str());
                } else {
                    toast_error("Couldn't make the clip", clip.error.c_str());
                }
            }
            /* "<name> is live" over the stream's title, likewise one at a
             * time. */
            else if (!evo_toast_visible() && evo_twitch::live_alerts_take(&notice, &detail))
                toast(notice.c_str(), detail.c_str());
        }
        {
            /* Chat follows the Twitch stream the player holds, the exit
             * dialog and the quality picker included. Not read while the
             * launcher's worker is inside startPlaybackSource(): the
             * controller has no locking (evo_twitch.h). A quality switch
             * restarting the channel keeps its chat meanwhile; any other
             * start has nothing Twitch playing yet. */
            const char* channel = evo_twitch_launcher_switching_channel();
            /* Home's muted picture has no chat, also on the frame its start
             * is back but not yet taken in by the launcher's pump. */
            if (!channel && !evo_twitch_preview_active() && !evo_twitch_preview_starting()) {
                const PlaybackSource* src = (m_playbackController && !evo_twitch_launcher_busy())
                                                ? m_playbackController->currentSource()
                                                : nullptr;
                if (src && evo_twitch::is_twitch_source(src->provider, src->item_id))
                    channel = src->item_id.c_str();
            }
            evo_twitch::chat_frame(channel, evo_twitch::settings().thirdPartyEmotes);
        }
        evo_twitch::images_frame();

        /* Non-blocking: wakes the log writer thread. This was a blocking
         * fflush+fsync to the USB stick the movie streams from, once a
         * second, and it stalled 4K playback 33-83 ms at a time. */
        if ((frame & 63) == 0) {
            evo_boot_log_kick();
        }

        static int jb_repaint = 0;
        if (evo_jailbreak_poll()) {
#ifdef EVO_APP_MODULE
            evo_data_path_rebind();
            EnsureDataDirectories();
            if (m_settingsService) {
                m_settingsService->loadSettings();
            }
            evo_bt("persistence: rebound to %s after late unjail", evo_data_dir());
            evo_boot_log_flush();
#endif
            toast_tech("Storage ready", nullptr);
            jb_repaint = 8;
        }

        /*
         * Log once when it is clear the promotion is not coming.
         *
         * The user-facing half of this is #nav-privilege-banner, driven from
         * evo_jailbreak_is_open() in Bridge.cpp, because a shut sandbox is a
         * condition that persists rather than an event - a toast that fades
         * after a few seconds leaves the app looking simply empty for as long
         * as it stays unusable.
         *
         * Still logged, and still late: evo_jailbreak_self() gets one short
         * attempt at boot because the daemon may not be polling yet, and
         * evo_jailbreak_poll() keeps re-dropping the request, so a promotion
         * landing a second or two in is the healthy case.
         */
        static bool s_jb_warned = false;
        if (!s_jb_warned && frame > 300 && !evo_jailbreak_is_open()) {
            s_jb_warned = true;
            evo_boot_log("jailbreak: sandbox still shut after %d frames - "
                         "no daemon answered the promotion request", frame);
            evo_boot_log_flush();
        }

        g_pace.mark(0);   /* pumps: net, usb remote, Twitch, jailbreak */

        /*
         * The system has the focus (its home screen, or a close from its
         * menu on the way): no input and no GPU work until the app has it
         * back (evo_sysui.h). On the way in the GPU is drained, so a close
         * meets an idle GPU, as after the soft close. Seen on hardware
         * (2026-10-19): the system takes the focus 1-2 s before it kills an
         * app, and this notices within a frame. Home's picture stops, as
         * nobody can see it; the pumps above keep running, and a stream in
         * the player plays on with its sound. Not while the PS5's own
         * keyboard is up: it is the system's, and it may hold the focus
         * while the app has to keep reading it.
         */
        {
            static bool s_sysui_parked = false;
            const bool lost = evo_sysui_focus_lost() == 1 && !evo_keyboard_is_native_active();
            if (lost && !m_softClose) {
                if (!s_sysui_parked) {
                    s_sysui_parked = true;
                    /* The GPU first: the kill can come well under a second
                     * after the focus moves (seen: about half a second). */
                    evo_agc_runtime_wait_idle(500);
                    evo_twitch_preview_stop();
                    evo_boot_log("sysui: focus left, drawing stopped and the GPU drained (screen %d)",
                                 static_cast<int>(m_screenManager->getCurrentScreenId()));
                    evo_boot_log_flush();
                }
                lastButtons = 0;
                usleep(10000);
                continue;
            }
            if (s_sysui_parked) {
                s_sysui_parked = false;
                jb_repaint = 8;   /* a few frames drawn whatever else is going on */
                evo_boot_log("sysui: focus back, drawing again");
                evo_boot_log_flush();
            }
        }
        // 1. Controller input & auto-repeat
        std::memset(&padData, 0, sizeof(padData));
        uint32_t pressed = 0;
        uint32_t released = 0;
        uint32_t held = 0;
        bool hasInput = false;

        if (m_padHandle >= 0 && scePadReadState(m_padHandle, &padData) == 0) {
            /*
             * Synthetic presses from the dev remote (`key <button>`), OR'd in
             * before anything reads the mask so they are indistinguishable
             * from a real tap: one frame set, cleared the next, which gives
             * the press edge here and the release on the following frame.
             * Compiles to nothing without --usb-remote.
             */
            padData.buttons |= evo_usb_remote_take_buttons();

            pressed = padData.buttons & ~lastButtons;
            released = ~padData.buttons & lastButtons;
            held = padData.buttons;

            // DualSense Left Analog Stick, normalized to -1..1
            float rawLx = (static_cast<float>(padData.leftStick.x) - 128.0f) / 128.0f;
            float rawLy = (static_cast<float>(padData.leftStick.y) - 128.0f) / 128.0f;

            // Synthesize stick tilt events into directional pad masks
            static bool s_stickLeft = false, s_stickRight = false, s_stickUp = false, s_stickDown = false;
            bool curStickLeft  = (rawLx < -0.55f);
            bool curStickRight = (rawLx >  0.55f);
            bool curStickUp    = (rawLy < -0.55f);
            bool curStickDown  = (rawLy >  0.55f);

            if (curStickLeft && !s_stickLeft)   pressed |= PadButtons::Left;
            if (curStickRight && !s_stickRight) pressed |= PadButtons::Right;
            if (curStickUp && !s_stickUp)       pressed |= PadButtons::Up;
            if (curStickDown && !s_stickDown)   pressed |= PadButtons::Down;

            if (curStickLeft)  held |= PadButtons::Left;
            if (curStickRight) held |= PadButtons::Right;
            if (curStickUp)    held |= PadButtons::Up;
            if (curStickDown)  held |= PadButtons::Down;

            s_stickLeft = curStickLeft;
            s_stickRight = curStickRight;
            s_stickUp = curStickUp;
            s_stickDown = curStickDown;

            evo_input_update(&evo_pad_state, padData.buttons, static_cast<uint64_t>(now_ms()));

            // Synthesize directional repeat events into pressed mask
            if (evo_input_fired(&evo_pad_state, EVO_ACT_UP))    pressed |= PadButtons::Up;
            if (evo_input_fired(&evo_pad_state, EVO_ACT_DOWN))  pressed |= PadButtons::Down;
            if (evo_input_fired(&evo_pad_state, EVO_ACT_LEFT))  pressed |= PadButtons::Left;
            if (evo_input_fired(&evo_pad_state, EVO_ACT_RIGHT)) pressed |= PadButtons::Right;

            // Touchpad gesture and left/right click synthesis
            static uint16_t s_lastTouchX = 960;
            static bool s_touchActive = false;
            static uint16_t s_swipeStartX = 0;
            static uint16_t s_swipeStartY = 0;
            static uint64_t s_swipeStartTime = 0;
            static bool s_swipeTriggered = false;

            const uint64_t curPadTime = static_cast<uint64_t>(now_ms());

            if (padData.touchData.touchNum > 0) {
                uint16_t tx = padData.touchData.touch[0].x;
                uint16_t ty = padData.touchData.touch[0].y;
                s_lastTouchX = tx;

                if (!s_touchActive) {
                    s_touchActive = true;
                    s_swipeStartX = tx;
                    s_swipeStartY = ty;
                    s_swipeStartTime = curPadTime;
                    s_swipeTriggered = false;
                } else if (!s_swipeTriggered && !(pressed & PadButtons::TouchPad)) {
                    int dx = static_cast<int>(tx) - static_cast<int>(s_swipeStartX);
                    int dy = static_cast<int>(ty) - static_cast<int>(s_swipeStartY);
                    uint64_t dt = curPadTime - s_swipeStartTime;

                    // Swipe: >= 250 px horizontally within 500 ms, with horizontal travel > 1.5 * vertical
                    if (dt <= 500 && std::abs(dx) >= 250 && std::abs(dx) > (std::abs(dy) * 3 / 2)) {
                        if (dx < 0) {
                            pressed |= PadButtons::TouchPadLeft;
                            evo_bt("touchpad: swipe left (dx=%d)", dx);
                        } else {
                            pressed |= PadButtons::TouchPadRight;
                            evo_bt("touchpad: swipe right (dx=%d)", dx);
                        }
                        s_swipeTriggered = true;
                    }
                }
            } else {
                s_touchActive = false;
                s_swipeTriggered = false;
            }

            // Touchpad physical click handling:
            if (pressed & PadButtons::TouchPad) {
                if (s_lastTouchX < 960) {
                    pressed |= PadButtons::TouchPadLeft;
                    evo_bt("touchpad: click left (x=%u)", s_lastTouchX);
                } else {
                    pressed |= PadButtons::TouchPadRight;
                    evo_bt("touchpad: click right (x=%u)", s_lastTouchX);
                }
            }
            if (held & PadButtons::TouchPad) {
                held |= (s_lastTouchX < 960 ? PadButtons::TouchPadLeft : PadButtons::TouchPadRight);
            }
            if (released & PadButtons::TouchPad) {
                released |= (PadButtons::TouchPadLeft | PadButtons::TouchPadRight);
            }

            hasInput = (pressed != 0 || released != 0 || evo_input_any(&evo_pad_state));
            if (hasInput) {
                evo::animation::AnimationManager::getInstance().triggerTransition(350.0);
            }

            /*
             * Either stick click captures the screen, and is swallowed so no
             * screen sees it as a normal press.
             *
             * It was bound to L3 alone and never once fired: across a logged
             * session the pad reported 0x0004 (R3) for the stick click and
             * 0x0002 (L3) not at all. Accepting both means it works whichever
             * bit this pad and firmware decide to send.
             *
             * Dev builds only (--usb-remote). In a release build a stick
             * click does nothing: it is still swallowed, so no screen sees
             * it, but nothing is captured.
             */
#if defined(EVO_USB_REMOTE)
            if (pressed & (PadButtons::L3 | PadButtons::R3)) {
                evo_bt("screenshot: stick click pressed=%#010x", pressed);
                evo_boot_log_flush();
                std::string shotPath;
                if (evo_capture_screenshot(shotPath)) {
                    const char* name = std::strrchr(shotPath.c_str(), '/');
                    toast_success("Screenshot saved", name ? name + 1 : shotPath.c_str());
                    {
                        char note[160];
                        std::snprintf(note, sizeof note, "EVO: screenshot saved - %s",
                                      name ? name + 1 : shotPath.c_str());
                        notifyOnScreen(note);
                    }
                    evo_bt("screenshot: wrote %s", shotPath.c_str());
                    evo_boot_log_flush();
                    evo_feedback(EVO_FB_CONFIRM);
                } else {
                    toast_error("Couldn't take a screenshot", nullptr);
                    notifyOnScreen("EVO: screenshot failed");
                    evo_feedback(EVO_FB_BOUNDARY);
                }
                pressed &= ~(PadButtons::L3 | PadButtons::R3);
            }
#else
            pressed &= ~(PadButtons::L3 | PadButtons::R3);
#endif

            if (evo_keyboard_is_open()) {
                evo_keyboard_update();
                if (pressed) {
                    evo_keyboard_handle_input(pressed);
                }
            } else {
                /* Not while a Twitch stream is starting: no screen may start
                 * or stop playback under its worker (evo_twitch.h). Home's
                 * picture is the exception: what a screen can do then waits
                 * for its start, so the pad stays alive. */
                if (hasInput && (!evo_twitch_launcher_busy() || evo_twitch_preview_starting())) {
                    m_screenManager->handleInput(pressed, held, released);
                }
            }

            lastButtons = padData.buttons;
        }

        /*
         * 2. Update animation engine and screen logic.
         *
         * Measured, not assumed: a hardcoded 16.667 ran every animation in
         * slow motion whenever the loop dropped below 60 (a seek settle sits
         * nearer 27). Clamped so a long stall - a file open, a thumbnail
         * decode - cannot teleport an animation to its end.
         */
        double frameDeltaMs = 16.667;
        {
            static uint64_t s_lastTickUs = 0;
            struct timeval tvNow;
            gettimeofday(&tvNow, nullptr);
            uint64_t nowUs = static_cast<uint64_t>(tvNow.tv_sec) * 1000000ULL +
                             static_cast<uint64_t>(tvNow.tv_usec);
            if (s_lastTickUs != 0) {
                frameDeltaMs = static_cast<double>(nowUs - s_lastTickUs) / 1000.0;
                if (frameDeltaMs < 1.0)   frameDeltaMs = 1.0;
                if (frameDeltaMs > 100.0) frameDeltaMs = 100.0;
            }
            s_lastTickUs = nowUs;
        }
        g_pace.mark(1);   /* input */
        evo::animation::AnimationManager::getInstance().update(frameDeltaMs);
        m_screenManager->update(frameDeltaMs);
        g_pace.mark(2);   /* update */

        // 3. Determine if graphics needs to render/present
        bool isPlayer = (m_screenManager->getCurrentScreenId() == ScreenId::Player);
        /* Home's muted hero. The picture is the video quad, so player mode
         * stays on while that stream is up even though this screen is not
         * the player. Decode already follows evo_pb_presenting(), which the
         * launcher sets for the preview before the first sample. */
        const bool preview = !isPlayer && evo_twitch_preview_active();
        const bool showVideo = isPlayer || preview;

        /* The player, or the picture inside Home (mode 2: no flash of both
         * scanouts on the way in, see evo_agc_runtime.h). */
        const int videoMode = isPlayer ? 1 : preview ? 2 : 0;
        static int s_video_mode = 0;
        if (videoMode != s_video_mode) {
            evo_agc_runtime_set_player_mode(videoMode);
            s_video_mode = videoMode;
        }

        /* The hero's art as fractions of the 1920x1080 page. Before
         * frame_begin: a new box marks both scanouts dirty, and that clear
         * has to land before the blit. */
        if (preview && !m_softClose) {
            evo_agc_runtime_set_video_rect(EVO_HUI_TWITCH_HERO_X / 1920.0f, EVO_HUI_TWITCH_HERO_Y / 1080.0f,
                                           EVO_HUI_TWITCH_HERO_W / 1920.0f, EVO_HUI_TWITCH_HERO_H / 1080.0f);
        }

        bool hasAnim = evo::animation::AnimationManager::getInstance().hasActiveAnimations();
        int uiActive = (frame < 10) || showVideo || hasInput || hasAnim ||
                       evo_rmlui_needs_frame() || (jb_repaint > 0);
        if (jb_repaint > 0) jb_repaint--;
        evo_rmlui_set_active(uiActive);

        if (frame < 5) {
            evo_bt("frame %d: uiActive=%d isPlayer=%d hasInput=%d", frame, uiActive, isPlayer ? 1 : 0, hasInput ? 1 : 0);
            evo_boot_log_flush();
        }

        if (uiActive && !isPlayer) {
            if (frame < 5) {
                evo_bt("frame %d: frame_begin", frame);
                evo_boot_log_flush();
            }
            evo_agc_runtime_frame_begin();
            if (frame < 5) {
                evo_bt("frame %d: frame_begin done", frame);
                evo_boot_log_flush();
            }
            /*
             * Only the CPU rasteriser reads this buffer back. On the console
             * RmlUi renders through the AGC interface and RenderCachedScreen
             * does `(void)framebuffer`, so the 8.3 MB clear was pure cost on
             * every menu frame. The allocation stays - the render entry points
             * still reject a null pointer.
             */
            if (m_uiScratch && evo_rmlui_blit_mode()) {
                std::memset(m_uiScratch, 0, static_cast<size_t>(DisplayWidth) * DisplayHeight * 4u);
            }
        }

        g_pace.mark(3);   /* decide: player mode, uiActive */
        // 4. Video Quad blit & Screen rendering
        bool swap = false;
        static int64_t s_last_pts = -1;
        if (!showVideo) {
            s_last_pts = -1;
            g_pace.stop();
        }

        if (isPlayer) {
            /* Heap snapshot every 2 s of playback (PlaybackController.cpp,
             * evo_log_alloc_state). The open/stop lines bracket a file; this is
             * what survives when the file never reaches stop. */
            {
                static auto s_alloc_next = std::chrono::steady_clock::time_point{};
                const auto now = std::chrono::steady_clock::now();
                if (now >= s_alloc_next) {
                    s_alloc_next = now + std::chrono::seconds(2);
                    evo_log_alloc_state("play");
                }
            }
            pp_video_frame f;
            std::memset(&f, 0, sizeof(f));
            int have = (pp_playback_get_video_frame(&g_pp_pb, &f) && f.ready);
            g_pace.mark(4);   /* fetch: alloc snapshot + get the video frame */
            int64_t current_pts = g_pp_pb.display_pts_us;
            bool new_frame = (current_pts != s_last_pts);

            /* Chat beside the video: the picture goes into the box left of
             * the panel while a Twitch stream plays with chat in side mode,
             * and over the whole screen otherwise. PlayerScreen::render asks
             * the same question this frame (evo_twitch_chat_mode.h), so the
             * panel and the picture agree. Every frame, before buffer_stale:
             * a change marks both buffers for a redraw, and the call costs
             * nothing when the box stays. While the launcher's worker is
             * inside startPlaybackSource() the source can't be read, so the
             * last decision holds. Left alone once the app is closing. */
            if (!m_softClose) {
                if (!evo_twitch_launcher_busy()) {
                    const PlaybackSource* src =
                        m_playbackController ? m_playbackController->currentSource() : nullptr;
                    const bool playing = src && evo_twitch::is_twitch_source(src->provider, src->item_id);
                    g_chat_side_video = evo_twitch::chat_mode_drawn(playing) == twitch::ChatMode::Side;
                }
                float vx, vy, vw, vh;
                evo_twitch::chat_video_box(g_chat_side_video ? twitch::ChatMode::Side : twitch::ChatMode::Off,
                                           &vx, &vy, &vw, &vh);
                evo_agc_runtime_set_video_rect(vx, vy, vw, vh);
            }

            /* isPlayer came from getCurrentScreenId(), so the type is already
             * proved - no need to pay for RTTI once a frame. */
            auto playerScreen = static_cast<PlayerScreen*>(m_screenManager->getCurrentScreen());
            bool overlay_active = playerScreen && playerScreen->hasActiveOverlay();
            bool is_paused = m_playbackController && m_playbackController->isPaused();
            bool is_scrubbing = m_playbackController && m_playbackController->isScrubbing();
            bool toast_visible = evo_toast_visible();

            /*
             * Redraw the quad whenever the buffer we are about to draw into
             * does not already hold this frame. Two scanout buffers and a
             * player-mode frame_begin that skips the clear mean a present that
             * skipped the blit shows the picture from two presents ago, so any
             * source slower than the panel - 24/25/30 fps, a paused picture,
             * the settle after a seek - flicked between the current frame and
             * the previous one. See evo_agc_runtime_video_slot_stale.
             */
            bool buffer_stale = have && evo_agc_runtime_video_slot_stale(current_pts);

            bool should_render = (have && new_frame) || buffer_stale || overlay_active || is_paused || is_scrubbing || toast_visible || g_pp_pb.seek_discarding;

            static int s_player_render_log = 10;
            if (s_player_render_log > 0 && new_frame && have) {
                s_player_render_log--;
                evo_boot_log("app video blit: have=%d ready=%d new_frame=%d pts=%lld y=%p uv=%p %ux%u (coded %ux%u)",
                             have, f.ready, new_frame, (long long)current_pts,
                             (void*)f.y, (void*)f.uv, f.disp_w, f.disp_h, f.coded_w, f.coded_h);
                evo_boot_log_flush();
            }

            if (should_render && have) {
                s_last_pts = current_pts;
                int view_mode = 0;
                if (m_playbackController) {
                    view_mode = static_cast<int>(m_playbackController->getViewMode());
                }
                int is_direct = (evo_pb_active_backend() == EVO_VDEC_BACKEND_NATIVE && !f.held && f.uv != nullptr) ? 1 : 0;
                if (new_frame) g_pace.new_frame();
                const auto blit_t0 = PaceTrace::clk::now();
                evo_agc_blit_yuv(f.y, f.y_pitch, f.uv, f.uv_pitch,
                                 f.u, f.u_pitch, f.v, f.v_pitch,
                                 static_cast<int>(f.coded_w), static_cast<int>(f.coded_h),
                                 static_cast<int>(f.disp_w), static_cast<int>(f.disp_h),
                                 view_mode, f.ten_bit, f.color_trc,
                                 is_direct, current_pts, f.dovi);
                g_pace.blit(PaceTrace::ms(blit_t0, PaceTrace::clk::now()));
                swap = true;
            }

            if (m_uiScratch && should_render) {
                const auto ui_t0 = PaceTrace::clk::now();
                m_screenManager->render(m_uiScratch, DisplayWidth, DisplayHeight);
                g_pace.ui(PaceTrace::ms(ui_t0, PaceTrace::clk::now()));
                /*
                 * The video quad only repaints the image. Any OSD or scrub bar
                 * drawn on top of it - or over a letterbox bar - has to be
                 * erased before this buffer is used again, or the next frame
                 * draws on top of it: the OSD stayed on screen after it faded
                 * out.
                 */
                if (overlay_active || is_paused || is_scrubbing || toast_visible) {
                    evo_agc_runtime_note_ui_drawn();
                }
                swap = true;
            }
        } else {
            /* The hero quad, into the scanout, before the page composites
             * over it. The hole in the backdrop is transparent, so this is
             * what shows through. Same blit as the player; the box above
             * keeps it on the art. The page goes over the picture every
             * frame (note_ui_drawn below), so each buffer is cleared before
             * it is drawn into again and the picture is blitted afresh:
             * nothing translucent (a dialog's dim, the rail's, a toast)
             * piles up on it. */
            if (preview && !m_softClose) {
                pp_video_frame f;
                std::memset(&f, 0, sizeof(f));
                int have = (pp_playback_get_video_frame(&g_pp_pb, &f) && f.ready);
                int64_t current_pts = g_pp_pb.display_pts_us;
                bool new_frame = (current_pts != s_last_pts);
                bool buffer_stale = have && evo_agc_runtime_video_slot_stale(current_pts);
                if (have && (new_frame || buffer_stale)) {
                    s_last_pts = current_pts;
                    int is_direct = (evo_pb_active_backend() == EVO_VDEC_BACKEND_NATIVE && !f.held && f.uv != nullptr) ? 1 : 0;
                    evo_agc_blit_yuv(f.y, f.y_pitch, f.uv, f.uv_pitch,
                                     f.u, f.u_pitch, f.v, f.v_pitch,
                                     static_cast<int>(f.coded_w), static_cast<int>(f.coded_h),
                                     static_cast<int>(f.disp_w), static_cast<int>(f.disp_h),
                                     0, f.ten_bit, f.color_trc,
                                     is_direct, current_pts, f.dovi);
                    swap = true;
                }
            }
            if (m_uiScratch) {
                if (frame < 5) {
                    evo_bt("frame %d: screenManager render begin", frame);
                    evo_boot_log_flush();
                }
                m_screenManager->render(m_uiScratch, DisplayWidth, DisplayHeight);
                if (frame < 5) {
                    evo_bt("frame %d: screenManager render done", frame);
                    evo_boot_log_flush();
                }
            }

            // 5. Present if uiActive
            if (uiActive && m_uiScratch) {
                g_ps5_video_out_hdr = 0;
                if (evo_rmlui_blit_mode()) {
                    if (frame < 5) {
                        evo_bt("frame %d: agc_composite_bgra begin", frame);
                        evo_boot_log_flush();
                    }
                    evo_agc_composite_bgra(m_uiScratch, DisplayWidth, DisplayHeight, 1);
                    if (frame < 5) {
                        evo_bt("frame %d: agc_composite_bgra done", frame);
                        evo_boot_log_flush();
                    }
                    swap = true;
                } else {
                    swap = evo_rmlui_consume_drew();
                }
            }
            /* The page went over Home's picture: this buffer is cleared
             * before its next use (see the blit above). */
            if (preview && !m_softClose)
                evo_agc_runtime_note_ui_drawn();
        }

        /*
         * Draw the toast, over whatever the screen just drew.
         *
         * evo_toast.h says this is "called every frame by the main render
         * loop" and nothing called it at all - the only reference anywhere was
         * evo_toast_visible() being used as a render gate. So toast() set its
         * state, the state expired on schedule, and not one pixel was ever
         * drawn. That is why a screenshot appeared to do nothing, and why
         * "Decoder seek failed" and the rest have been silent too.
         *
         * It forces a present of its own: a toast is often the only thing that
         * changed, and without this it would wait for something else to
         * trigger a frame.
         */
        if (uiActive && evo_toast_visible()) {
            draw_prospero_toast(m_uiScratch);
            swap = true;
        }

        /*
         * The virtual keyboard, for the same reason and in the same place.
         *
         * evo_rmlui_render_keyboard() had exactly one caller -
         * evo_screen_keyboard() - and nothing called that, so the virtual
         * keyboard could not draw on ANY screen. It still opened and still
         * swallowed every button (the input dispatch routes everything to it
         * while it is up), so the symptom was a frozen-looking screen with no
         * prompt on it, hidden only because the native IME was the default.
         *
         * Skipped when the native IME is active: the system draws that dialog
         * itself, and drawing over it would be wrong.
         */
        if (uiActive && evo_keyboard_is_open() && !evo_keyboard_is_native_active()) {
            evo_screen_keyboard(m_uiScratch);
            m_keyboardDrawn = true;
            swap = true;
        } else if (uiActive && m_keyboardDrawn) {
            /*
             * One last call on the frame after it closes. evo_screen_keyboard()
             * hides its RmlUi document when the keyboard is shut, but this gate
             * meant that branch could never run: the document stayed visible,
             * so #115's `ui` readback went on reporting a keyboard modal over
             * every screen that followed - the dev remote's view of the UI was
             * wrong from the first text entry onwards.
             */
            evo_screen_keyboard(m_uiScratch);
            m_keyboardDrawn = false;
            swap = true;
        }

        // 5. Present if swap requested
        if (swap && uiActive) {
            if (frame < 5) {
                evo_bt("frame %d: present begin", frame);
                evo_boot_log_flush();
            }
            static int s_present_player_log = 10;
            bool log_this_present = false;
            if (isPlayer && s_present_player_log > 0) {
                s_present_player_log--;
                log_this_present = true;
                evo_boot_log("app present begin isPlayer=1");
                evo_boot_log_flush();
            }
            g_pace.mark(5);   /* draw: blit, UI, toast, keyboard */
            const auto present_t0 = PaceTrace::clk::now();
            evo_agc_runtime_present();
            g_pace.mark(6);   /* present, incl. the flip wait */
            if (isPlayer) g_pace.present(present_t0, PaceTrace::clk::now());
            evo_rmlui_end_frame();

#if defined(EVO_APP_MODULE) || defined(EVO_TARGET_PS5)
            /*
             * Dismiss the system splash, once EVO has something real on
             * screen behind it.
             *
             * The shell shows sce_sys/pic1.png while a title starts and leaves
             * it up until the app says it is ready. Nothing here ever did,
             * which went unnoticed for as long as the project shipped no
             * pic0/pic1 at all - with no artwork there was no splash, so there
             * was nothing to dismiss. Adding the background art in 1584ddd
             * gave the shell something to show, and the player then rendered
             * happily underneath it forever: the frame loop presents at 60 fps
             * with zero flip failures while the panel shows the splash.
             *
             * Held until a frame has presented so the splash gives way
             * to a drawn UI rather than to one black frame.
             */
            static bool s_splash_hidden = false;
            if (!s_splash_hidden && frame >= 1) {
                s_splash_hidden = true;
                const int rc = sceSystemServiceHideSplashScreen();
                evo_boot_log("splash: hide rc=%d (0 = dismissed)", rc);
                evo_boot_log_flush();
            }
#endif
            if (log_this_present) {
                evo_boot_log("app present done isPlayer=1");
                evo_boot_log_flush();
            }
            if (frame < 5) {
                evo_bt("frame %d: present done", frame);
                evo_boot_log_flush();
            }
        }

#if defined(EVO_APP_MODULE) || defined(EVO_TARGET_PS5)
        /* Fallback: if present didn't dismiss it by frame 20, force-dismiss splash */
        static bool s_splash_fallback_hidden = false;
        if (!s_splash_fallback_hidden && frame >= 20) {
            s_splash_fallback_hidden = true;
            const int rc = sceSystemServiceHideSplashScreen();
            evo_boot_log("splash: fallback hide rc=%d", rc);
            evo_boot_log_flush();
        }
#endif

        evo_perf_monitor_tick(0.0);
        if (!swap) {
            /*
             * Nothing was presented. A 1 ms spin here meant an idle menu ran
             * this loop ~1000 times a second, and every pass rebuilt each
             * screen's RmlUi parameter block - strings and all - for a frame
             * RenderCachedScreen then discarded because the UI was inactive.
             * Idle backs off to display cadence; an active frame that simply
             * had nothing to draw still retries promptly.
             */
            usleep(uiActive ? 1000 : 8000);
        }
    }

    shutdown();
    return 0;
}

} // namespace evo
