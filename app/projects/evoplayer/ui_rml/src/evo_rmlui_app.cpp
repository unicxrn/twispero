#include "evo_rmlui_app.h"
#include "evo_about_text.hpp"
#if defined(EVO_AGC_DEVICE)
#include "evo_hui.h"
#endif
#include "evo_rmlui_prof.h"
#include "evo_metrics.h"   /* EVO_UI_DESIGN_W/H - the dp authoring canvas */
#include "prospero_thumbnail.h"   /* #32 scrub preview: request/serial/snapshot */
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include <cstring>
#include <cstdarg>

/*
 * #71 REDUX - NO libc++ STREAMS IN THE APP MODULE.
 *
 * std::ostringstream constructs the global std::locale on first use, and in
 * the native-app CRT that reads [null + 0x48] and takes the process down with
 * signal 11. That is the exact fault df7cbf2 recorded for <iostream>'s static
 * initialiser:
 *
 *   std::ios_base::Init::Init() -> std::locale::locale() -> read [null+0x48]
 *
 * Dropping <iostream> in #71 only moved it from load time to first use - every
 * ostringstream in here was still a live landmine. And it is LAYOUT-SENSITIVE,
 * so a build that happens to survive proves nothing: #90's extra .text shifted
 * the addresses and EVO began crashing on the first poster it registered,
 * inside ArtSource()'s stream. Two hardware runs, "CRASH signal=11 addr=48".
 *
 * So: snprintf everywhere, and <sstream>/<iomanip> stay out of this file.
 */
/* The path of the file playing, owned by Bridge.cpp. The scrub preview worker
 * takes a path rather than a handle, so it needs this rather than anything out
 * of EvoPlaybackState. */
extern "C" char current_media_path[768];

static std::string evo_fmt(const char* fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return std::string(buf);
}
#include <cmath>
#include <chrono>

/* EVO_AGC_DEVICE is the console build. The host preview harness
 * (tools/uiview_playback_rml.sh) compiles this same file without it and gets
 * the CPU rasteriser - that is the only other configuration there is. */
#if defined(EVO_AGC_DEVICE)
#include "evo_rmlui_render_agc.h"
#include "evo_agc_runtime.h"
#include "evo_boot_log.h"
#else
static inline int evo_agc_runtime_is_120hz(void) { return 0; }
#endif

/* No <iostream>: its static init (ios_base::Init -> std::locale::locale())
 * crashes at load in the app module's custom CRT _init() pass, layout-
 * sensitively - #71. These diagnostics go to stderr (klog) via C stdio. */

/* FrameBegin/FrameEnd bracket every Context::Render(): no-ops on the CPU
 * rasteriser, real work on the AGC interface. Mark that a Context::Render()
 * actually reached the scanout this frame, so the device loop knows to flip. */
#if defined(EVO_AGC_DEVICE)
#define EVO_MARK_DREW() (m_drew = true)
#else
#define EVO_MARK_DREW() ((void)0)
#endif

#ifdef EVO_RML_PROFILE
#define EVO_PROF_CTX_RENDER() do {                                      \
        double _c0 = evo_prof_now_ms(); m_context->Update();            \
        double _c1 = evo_prof_now_ms();                                 \
        m_render->FrameBegin(); m_context->Render(); m_render->FrameEnd();\
        double _c2 = evo_prof_now_ms(); EVO_MARK_DREW();             \
        g_evo_rml_prof.update_ms += _c1 - _c0; g_evo_rml_prof.update_n++;\
        g_evo_rml_prof.render_ms += _c2 - _c1; g_evo_rml_prof.render_n++;\
    } while (0)
#else
#define EVO_PROF_CTX_RENDER() do {                                      \
        m_context->Update();                                           \
        m_render->FrameBegin(); m_context->Render(); m_render->FrameEnd();\
        EVO_MARK_DREW();                                            \
    } while (0)
#endif

/* Never destroyed - same reason as Application::getInstance(); its teardown
 * is done explicitly by evo_rmlui_shutdown() from Application::shutdown(),
 * and letting it also unwind at exit crashed QUIT EVO. */
EvoRmlApp& EvoRmlApp::Instance() {
    static EvoRmlApp* p_instance = new EvoRmlApp();
    EvoRmlApp& instance = *p_instance;
    return instance;
}

EvoRmlApp::EvoRmlApp() {
}

EvoRmlApp::~EvoRmlApp() {
    Shutdown();
}

static std::string format_time(double seconds) {
    if (seconds < 0) seconds = 0;
    int s = (int)std::floor(seconds);
    int h = s / 3600;
    int m = (s % 3600) / 60;
    int sec = s % 60;
    if (h > 0) return evo_fmt("%02d:%02d:%02d", h, m, sec);
    return evo_fmt("%02d:%02d", m, sec);
}

/* The app's name for the safe-to-close page's small capitals: ASCII letters
 * upper-cased, every other byte (UTF-8 included) kept as it is. */
static std::string upper_ascii(const char* s) {
    std::string out(s ? s : "");
    for (char& c : out)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return out;
}

static std::string to_hex_rgb(uint32_t col) {
    uint8_t r = col & 0xFF;
    uint8_t g = (col >> 8) & 0xFF;
    uint8_t b = (col >> 16) & 0xFF;
    char buf[16];
    snprintf(buf, sizeof(buf), "#%02x%02x%02x", r, g, b);
    return std::string(buf);
}

/*
 * A label colour that stays readable on `bg`.
 *
 * Every theme accent is a light colour - Midnight #FFCD00, Carbon #F2F2F5,
 * Ember #FFA528, Aurora #3DF5C0 - so a pill that fills itself with the accent
 * and leaves its label white is low-contrast in all four themes, and in Carbon
 * it is white on near-white: the About version and the PAUSED badge were both
 * effectively invisible. Derive the ink from the fill instead of assuming it.
 */
static std::string ink_on(uint32_t bg) {
    const double r = (bg & 0xFF) / 255.0;
    const double g = ((bg >> 8) & 0xFF) / 255.0;
    const double b = ((bg >> 16) & 0xFF) / 255.0;
    const double lum = 0.2126 * r + 0.7152 * g + 0.0722 * b;   // Rec. 709
    return lum > 0.45 ? "#0b1017" : "#ffffff";
}

static std::string to_hex_rgba(uint32_t col) {
    uint8_t r = col & 0xFF;
    uint8_t g = (col >> 8) & 0xFF;
    uint8_t b = (col >> 16) & 0xFF;
    uint8_t a = (col >> 24) & 0xFF;
    char buf[16];
    snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x", r, g, b, a);
    return std::string(buf);
}

void EvoRmlApp::SetTheme(const EvoThemeColors& theme) {
    m_theme = theme;
    m_theme_generation++;
}

void EvoRmlApp::SetImageColor(Rml::Element* el, const std::string& color) {
    if (!el) return;
    auto it = m_image_color_cache.find(el);
    if (it != m_image_color_cache.end() && it->second == color) return;
    el->SetProperty("image-color", color);
    m_image_color_cache[el] = color;
}

/*
 * Step 1 of docs/evo-pro/gpu-rendering-plan.md — see the comment on m_surface
 * in evo_rmlui_app.h. Rasterise a full-screen menu document into the retained
 * m_surface only when it actually changed, then blit that surface into the
 * caller's (rotating) VideoOut buffer every frame.
 */
void EvoRmlApp::RenderCachedScreen(int screen_id, uint32_t* framebuffer,
                                   int width, int height)
{
#if defined(EVO_AGC_DEVICE)
    /* GL-3 (#79) / AGC. Both modes: only touch anything on an "active" frame - the
     * device loop decided this iteration is a redraw (GlNeedsFrame); otherwise
     * the front buffer holds. */
    if (!m_gl_active)
        return;
    if (!m_blit_mode) {
        /* Mode B / AGC: RmlUi renders itself straight to the hardware default framebuffer;
         * main.c presents. No m_surface. */
        (void)framebuffer;
        m_render->SetFramebuffer(nullptr);
        m_render->SetDimensions(width, height);
        EVO_PROF_CTX_RENDER();   /* also sets m_drew */
        m_cached_screen = screen_id;
        return;
    }
    /* Mode A (default GL): CPU coverage rasteriser into m_surface -> `framebuffer`
     * (the loop's scratch); main.c uploads it as one GL quad. Falls through to
     * the shared cached-raster path below. */
#endif
    {
    const size_t px = (size_t)width * (size_t)height;

    bool resized = (width != m_surface_w || height != m_surface_h);
    if (resized) {
        m_surface.assign(px, 0xFF000000u);
        m_surface_w = width;
        m_surface_h = height;
        m_cached_screen = -1;
    }

    bool screen_changed = (screen_id != m_cached_screen);

    if (m_frame_dirty || screen_changed || resized) {
        m_render->SetFramebuffer(m_surface.data());
        m_render->SetDimensions(width, height);
        EVO_PROF_CTX_RENDER();
        m_frame_dirty = false;
        m_cached_screen = screen_id;
    }

    if (framebuffer != m_surface.data())
        std::memcpy(framebuffer, m_surface.data(), px * sizeof(uint32_t));
    }
#if defined(EVO_AGC_DEVICE)
    m_drew = true;   /* Mode A rasterised into `framebuffer` this frame */
#endif
}

/* See the declaration in evo_rmlui_app.h for why this exists. */
void EvoRmlApp::ShowOnlyScreen(Rml::ElementDocument* keep)
{
    Rml::ElementDocument* const screens[] = {
        m_list_doc, m_playback_doc, m_dialog_doc,
        m_settings_doc, m_about_doc,
        m_closed_doc, m_twitch_fallback_doc,
    };
    for (Rml::ElementDocument* doc : screens) {
        if (!doc || doc == keep) continue;
        doc->Hide();
    }
    if (keep) keep->Show();
}

bool EvoRmlApp::GlNeedsFrame()
{
    /* Retained-mode + ps5-opengl can't re-raster + MSAA-resolve + swap at 60 Hz,
     * so the device loop only redraws on change. Marquee / RCSS animation is
     * deferred to GL-5; the overlay docs that animate (toast slide, dialog) are
     * caught by the IsVisible checks. Called exactly once per frame by main.c. */
    if (m_gl_warmup > 0) { m_gl_warmup--; return true; }
    if (m_frame_dirty) return true;
#if defined(EVO_AGC_DEVICE)
    /* ps5-homebrew-ui screens animate continuously (springs, entrances). */
    if (evo_hui_wants_frame()) return true;
#endif
    if (m_toast_doc && m_toast_doc->IsVisible()) return true;
    if (m_dialog_doc && m_dialog_doc->IsVisible()) return true;
    /* Dev debug overlay: keep the menu FPS number counting on an idle screen -
     * force a redraw ~2 Hz while it is up. */
    if (m_debug_visible) {
        long long now = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (now - m_debug_tick_ms >= 500) { m_debug_tick_ms = now; return true; }
    }
    return false;
}

bool EvoRmlApp::Initialize(int width, int height) {
    if (m_initialized) return true;

    m_width = width;
    m_height = height;

    /* Force the first RenderCachedScreen call after a (re)init to rasterise. */
    m_surface_w = m_surface_h = 0;
    m_cached_screen = -1;
    m_frame_dirty = true;

    m_system = std::make_unique<EvoSystemInterface>();

#ifdef EVO_AGC_DEVICE
    evo_log("RmlUi AGC: EVO_AGC_DEVICE compiled in, is_active=%d", evo_agc_runtime_is_active());
    evo_log_flush();
    if (evo_agc_runtime_is_active()) {
        int w = 0, h = 0;
        evo_agc_runtime_get_size(&w, &h);
        if (w <= 0 || h <= 0) {
            w = width;
            h = height;
        }
        auto agc = std::make_unique<EvoRenderInterfaceAGC>(w, h);
        if (agc->Ok()) {
            m_render = std::move(agc);
            m_blit_mode = false;
            evo_log("RmlUi AGC: bare-metal GPU render interface active (%dx%d)", w, h);
            evo_log_flush();
        } else {
            evo_log("RmlUi AGC: failed to construct EvoRenderInterfaceAGC");
            evo_log_flush();
        }
    }
#endif
    if (!m_render)
        m_render = std::make_unique<EvoRenderInterface>(width, height);
    /* #60: must be registered before Rml::Initialise() so every subsequent
     * LoadFontFace/LoadDocument call resolves through the embedded bundle
     * first, never touching the /app0 sandbox's broken directory traversal. */
    m_file_interface = std::make_unique<EvoRmlFileInterface>();

    Rml::SetSystemInterface(m_system.get());
    Rml::SetRenderInterface(m_render->AsRml());
    Rml::SetFileInterface(m_file_interface.get());

    if (!Rml::Initialise()) {
        fprintf(stderr, "[EVO RmlUi] Failed to initialise RmlUi core!\n");
        return false;
    }

    /* #60: "/app0/assets/fonts/" resolves through the embedded bundle
     * (evo_rmlui_fileinterface.h) before any real filesystem call happens, so
     * the /app0 sandbox's broken directory traversal never comes into play.
     * The remaining entries are host/dev fallbacks (fopen against loose
     * files) for when the bundle hasn't been (re)generated. The old
     * /data/ps5twitch/app/ and /data/homebrew/EVOPlayer/ search paths - the
     * #44 out-of-band FTP sync target - are gone: that sync is removed
     * (deploy-app.sh), and shipping a self-contained .ffpfsc means those
     * loose console-side copies must have zero effect on rendering. */
    std::vector<std::string> font_prefixes = {
        "/app0/assets/fonts/",
        "assets/fonts/",
        "projects/evoplayer/assets/fonts/",
        "/workspace/projects/evoplayer/assets/fonts/"
    };

    for (const auto& p : font_prefixes) {
        /* Lato's Latin set (Western and Central European). The RmlUi screens
         * are the fallback for the kit's; they have no face for other
         * scripts, and FreeType draws nothing for a glyph the face lacks. */
        if (Rml::LoadFontFace(p + "LatoLatin-Regular.ttf", true)) {
            Rml::LoadFontFace(p + "LatoLatin-Bold.ttf", true);
            fprintf(stderr, "[EVO RmlUi] Loaded font face from %s\n", p.c_str());
            break;
        }
    }

    m_dp_ratio = (width > 0) ? (float)width / (float)EVO_UI_DESIGN_W : 1.0f;
    m_context = Rml::CreateContext("main_context", Rml::Vector2i(width, height));
    if (!m_context) {
        fprintf(stderr, "[EVO RmlUi] Failed to create RmlUi context!\n");
        Rml::Shutdown();
        return false;
    }

    /*
     * Every stylesheet is authored against the EVO_UI_DESIGN_W x _H canvas
     * (ui/include/evo_metrics.h) and expresses its geometry in dp, so one
     * ratio scales the whole UI to whatever the panel is running at: layout,
     * glyph rasterisation and corner tessellation all happen at the real
     * pixel size instead of being upscaled from a 1080p image. At 1920 wide
     * the ratio is exactly 1 and every dp resolves to the px it replaced,
     * which is what keeps the host renderer shot.sh diff baselines valid.
     */
    m_context->SetDensityIndependentPixelRatio(m_dp_ratio);

    std::vector<std::string> rml_prefixes = {
        "/app0/assets/rml/",     /* #60: resolves through the embedded bundle */
        "assets/rml/",
        "projects/evoplayer/assets/rml/",
        "/workspace/projects/evoplayer/assets/rml/"
    };

    for (const auto& p : rml_prefixes) {
        if (!m_list_doc) {
            m_list_doc = m_context->LoadDocument(p + "list.rml");
            if (m_list_doc) m_list_doc->Hide();
        }
        if (!m_playback_doc) {
            m_playback_doc = m_context->LoadDocument(p + "playback.rml");
            if (m_playback_doc) m_playback_doc->Hide();
        }
        if (!m_dialog_doc) {
            m_dialog_doc = m_context->LoadDocument(p + "dialog.rml");
            if (m_dialog_doc) m_dialog_doc->Hide();
        }
        if (!m_settings_doc) {
            m_settings_doc = m_context->LoadDocument(p + "settings.rml");
            if (m_settings_doc) m_settings_doc->Hide();
        }
        if (!m_about_doc) {
            m_about_doc = m_context->LoadDocument(p + "about.rml");
            if (m_about_doc) m_about_doc->Hide();
        }
        if (!m_closed_doc) {
            m_closed_doc = m_context->LoadDocument(p + "closed.rml");
            if (m_closed_doc) m_closed_doc->Hide();
        }
        if (!m_twitch_fallback_doc) {
            m_twitch_fallback_doc = m_context->LoadDocument(p + "twitch_fallback.rml");
            if (m_twitch_fallback_doc) m_twitch_fallback_doc->Hide();
        }
        if (!m_nav_doc) {
            m_nav_doc = m_context->LoadDocument(p + "navbar.rml");
            if (m_nav_doc) m_nav_doc->Hide();
            /* The name beside the mark, from sce_sys/param.json. */
            if (Rml::Element* el = m_nav_doc ? m_nav_doc->GetElementById("nav-app-label") : nullptr)
                el->SetInnerRML(upper_ascii(::evo::about::kAppName));
        }
    }

    if (!m_list_doc) fprintf(stderr, "[EVO RmlUi] Failed to load list.rml!\n");
    if (!m_playback_doc) fprintf(stderr, "[EVO RmlUi] Failed to load playback.rml!\n");
    if (!m_dialog_doc) fprintf(stderr, "[EVO RmlUi] Failed to load dialog.rml!\n");
    if (!m_settings_doc) fprintf(stderr, "[EVO RmlUi] Failed to load settings.rml!\n");
    if (!m_about_doc) fprintf(stderr, "[EVO RmlUi] Failed to load about.rml!\n");
    if (!m_closed_doc) fprintf(stderr, "[EVO RmlUi] Failed to load closed.rml!\n");
    if (!m_twitch_fallback_doc) fprintf(stderr, "[EVO RmlUi] Failed to load twitch_fallback.rml!\n");
    if (!m_nav_doc) fprintf(stderr, "[EVO RmlUi] Failed to load navbar.rml!\n");

    /* #75: toast lives in its own context - see the comment on
     * EvoToastState for why. Its own tiny context, so a load failure here
     * is a missing notification, never a missing screen - non-fatal. */
    m_toast_context = Rml::CreateContext("toast_context", Rml::Vector2i(width, height));
    if (m_toast_context) {
        m_toast_context->SetDensityIndependentPixelRatio(m_dp_ratio);
        for (const auto& p : rml_prefixes) {
            if (m_toast_doc) break;
            m_toast_doc = m_toast_context->LoadDocument(p + "toast.rml");
        }
        if (m_toast_doc) {
            m_toast_doc->Hide();
        } else {
            fprintf(stderr, "[EVO RmlUi] Failed to load toast.rml!\n");
        }
    } else {
        fprintf(stderr, "[EVO RmlUi] Failed to create toast context!\n");
    }

    /* #81: virtual keyboard - own context (same rationale as the toast). A load
     * failure just means text entry falls back to nothing on screen, which is a
     * bug but not a crash - evo_keyboard.c still has the state. */
    m_keyboard_context = Rml::CreateContext("keyboard_context", Rml::Vector2i(width, height));
    if (m_keyboard_context) {
        m_keyboard_context->SetDensityIndependentPixelRatio(m_dp_ratio);
        for (const auto& p : rml_prefixes) {
            if (m_keyboard_doc) break;
            m_keyboard_doc = m_keyboard_context->LoadDocument(p + "keyboard.rml");
        }
        if (m_keyboard_doc) m_keyboard_doc->Hide();
        else fprintf(stderr, "[EVO RmlUi] Failed to load keyboard.rml!\n");
    } else {
        fprintf(stderr, "[EVO RmlUi] Failed to create keyboard context!\n");
    }

    /* Dev debug overlay (menu FPS pill) - own context, same rationale as the
     * toast. A load failure just means no menu FPS readout - never a crash. */
    m_debug_context = Rml::CreateContext("debug_context", Rml::Vector2i(width, height));
    if (m_debug_context) {
        m_debug_context->SetDensityIndependentPixelRatio(m_dp_ratio);
        for (const auto& p : rml_prefixes) {
            if (m_debug_doc) break;
            m_debug_doc = m_debug_context->LoadDocument(p + "debug.rml");
        }
        if (m_debug_doc) m_debug_doc->Hide();
        else fprintf(stderr, "[EVO RmlUi] Failed to load debug.rml!\n");
    } else {
        fprintf(stderr, "[EVO RmlUi] Failed to create debug context!\n");
    }

    m_initialized = true;
    fprintf(stderr, "[EVO RmlUi] Retained-mode Full Engine initialized successfully (%dx%d).\n",
            width, height);
    return true;
}

void EvoRmlApp::Shutdown() {
    if (!m_initialized) return;

    if (m_about_doc) {
        m_about_doc->Close();
        m_about_doc = nullptr;
    }

    if (m_closed_doc) {
        m_closed_doc->Close();
        m_closed_doc = nullptr;
    }

    if (m_settings_doc) {
        m_settings_doc->Close();
        m_settings_doc = nullptr;
    }

    if (m_dialog_doc) {
        m_dialog_doc->Close();
        m_dialog_doc = nullptr;
    }

    if (m_playback_doc) {
        m_playback_doc->Close();
        m_playback_doc = nullptr;
    }

    if (m_nav_doc) {
        m_nav_doc->Close();
        m_nav_doc = nullptr;
    }

    if (m_list_doc) {
        m_list_doc->Close();
        m_list_doc = nullptr;
    }

    if (m_context) {
        Rml::RemoveContext(m_context->GetName());
        m_context = nullptr;
    }

    if (m_toast_doc) {
        m_toast_doc->Close();
        m_toast_doc = nullptr;
    }

    if (m_toast_context) {
        Rml::RemoveContext(m_toast_context->GetName());
        m_toast_context = nullptr;
    }

    if (m_keyboard_doc) {
        m_keyboard_doc->Close();
        m_keyboard_doc = nullptr;
    }
    if (m_keyboard_context) {
        Rml::RemoveContext(m_keyboard_context->GetName());
        m_keyboard_context = nullptr;
    }

    if (m_debug_doc) {
        m_debug_doc->Close();
        m_debug_doc = nullptr;
    }
    if (m_debug_context) {
        Rml::RemoveContext(m_debug_context->GetName());
        m_debug_context = nullptr;
    }

    Rml::Shutdown();

    m_render.reset();
    m_system.reset();
    m_initialized = false;
}

/* ==========================================================================
 * Shared helpers
 * ========================================================================== */

static std::string pct_string(int permille) {
    double pct = permille / 10.0;
    if (pct < 0.0)   pct = 0.0;
    if (pct > 100.0) pct = 100.0;
    return evo_fmt("%.1f%%", pct);
}

/*
 * Name the texture for one art slot, registering the pixels with the render
 * interface if they have changed. RmlUi caches by source string, so the name
 * carries a generation counter: reusing it would serve the previous poster
 * forever, and the cover cache hands back the same buffer for different
 * files. Returns an empty string when there is no artwork.
 */
std::string EvoRmlApp::ArtSource(int slot, const uint32_t* pixels, int w, int h,
                                 const std::string& tag)
{
    if (slot < 0 || slot >= kArtSlots) return std::string();

    if (!pixels || w <= 0 || h <= 0) {
        if (!m_art_source[slot].empty()) {
            Rml::ReleaseTexture(m_art_source[slot], m_render->AsRml());
            m_render->DropMemoryTexture(m_art_source[slot]);
            m_art_source[slot].clear();
        }
        m_art_last_ptr[slot] = nullptr;
        m_art_last_tag[slot].clear();
        return std::string();
    }

    bool unchanged = !m_art_source[slot].empty() &&
                     pixels == m_art_last_ptr[slot] &&
                     w == m_art_last_dims[slot][0] &&
                     h == m_art_last_dims[slot][1] &&
                     tag == m_art_last_tag[slot];
    if (unchanged) return m_art_source[slot];

    if (!m_art_source[slot].empty()) {
        Rml::ReleaseTexture(m_art_source[slot], m_render->AsRml());
        m_render->DropMemoryTexture(m_art_source[slot]);
    }

    m_art_generation[slot]++;
    m_art_source[slot] = evo_fmt("evo:mem/art%d-%d", slot, m_art_generation[slot]);
    m_art_last_ptr[slot] = pixels;
    m_art_last_dims[slot][0] = w;
    m_art_last_dims[slot][1] = h;
    m_art_last_tag[slot] = tag;

    m_render->SetMemoryTexture(m_art_source[slot], pixels, w, h);
    return m_art_source[slot];
}

/*
 * Ping-pong scroll for a single-line (white-space: nowrap) text element whose
 * content overflows its parent's fixed-width overflow:hidden clip box,
 * matching the legacy evo_text_marquee: dwell at each end, constant px/sec
 * travel. Slides the element with a negative margin-left inside the parent
 * clip. Reads the post-layout width from the previous frame (one-frame lag,
 * invisible in motion). Call every frame the owning document is rendered.
 */
void EvoRmlApp::MarqueeTick(Rml::Element* text_el, bool active) {
    if (!text_el) return;
    Rml::Element* box_el = text_el->GetParentNode();
    if (!box_el) return;

    const float content = text_el->GetScrollWidth();   /* nowrap block = text width */
    const float box     = box_el->GetClientWidth();     /* the fixed clip window */
    const float travel  = content - box;

    MarqueeState& m = m_marquee[text_el];

    if (!active || travel <= 4.0f) {
        if (m.active) {
            text_el->SetProperty("margin-left", "0px");
            m.active = false;
        }
        return;
    }

    const double now = m_system ? m_system->GetElapsedTime() : 0.0;

    if (!m.active || std::fabs(m.travel - travel) > 1.0f) {
        m.active = true;
        m.start  = now;
        m.travel = travel;
    }

    const double PAUSE = 1.4;   /* seconds held at each end */
    const double SPEED = 85.0;  /* px per second */
    const double leg    = m.travel / SPEED;
    const double period = 2.0 * (leg + PAUSE);
    const double p      = std::fmod(now - m.start, period);

    double off;
    if (p < PAUSE)                    off = 0.0;
    else if (p < PAUSE + leg)         off = (p - PAUSE) * SPEED;
    else if (p < 2.0 * PAUSE + leg)   off = m.travel;
    else                             off = m.travel - (p - 2.0 * PAUSE - leg) * SPEED;

    if (off < 0.0)          off = 0.0;
    if (off > m.travel)     off = m.travel;

    text_el->SetProperty("margin-left", std::to_string(-(int)(off + 0.5)) + "px");
}

/* ==========================================================================
 * Generic list screen - the track picker
 * ========================================================================== */

void EvoRmlApp::UpdateListState(const EvoListState& state) {
    if (!m_initialized || !m_list_doc) return;
    if (state == m_last_list && m_theme_generation == m_theme_gen_list) return;
    m_theme_gen_list = m_theme_generation;
    m_frame_dirty = true;

    if (!m_version.empty()) {
        if (Rml::Element* vel = m_list_doc->GetElementById("list-footer-version"))
            vel->SetInnerRML(m_version);
    }

    m_last_list = state;

    const std::string accent   = to_hex_rgb(m_theme.accent);
    const std::string surface  = to_hex_rgba(m_theme.surface);
    const std::string surf_sel = to_hex_rgba(m_theme.surface_sel);
    const std::string border   = to_hex_rgba(m_theme.border);
    const std::string text_1   = to_hex_rgb(m_theme.text_primary);
    const std::string text_2   = to_hex_rgb(m_theme.text_secondary);
    const std::string text_3   = to_hex_rgb(m_theme.text_muted);

    auto el = [&](const std::string& id) { return m_list_doc->GetElementById(id); };

    if (Rml::Element* e = el("list-title"))    e->SetInnerRML(state.title);
    if (Rml::Element* e = el("list-subtitle")) e->SetInnerRML(state.subtitle);
    if (Rml::Element* e = el("list-indicator")) e->SetProperty("background-color", accent);

    /* "n OF m" only when the list is longer than the window — otherwise the
     * count is already on screen and the marker is noise. */
    if (Rml::Element* e = el("list-counter")) {
        if (state.total_count > (int)state.rows.size() && state.cursor_index >= 0) {
            e->SetInnerRML(
                evo_fmt("%d OF %d", state.cursor_index + 1, state.total_count));
            e->SetProperty("display", "block");
            e->SetProperty("color", text_3);
        } else if (state.total_count > 0) {
            e->SetInnerRML(evo_fmt("%d%s", state.total_count,
                                   state.total_count == 1 ? " ITEM" : " ITEMS"));
            e->SetProperty("display", "block");
            e->SetProperty("color", text_3);
        } else {
            e->SetProperty("display", "none");
        }
    }

    /* Empty state replaces the rows outright rather than sitting under them. */
    if (Rml::Element* e = el("list-rows"))
        e->SetProperty("display", state.is_empty ? "none" : "flex");
    if (Rml::Element* e = el("list-empty"))
        e->SetProperty("display", state.is_empty ? "flex" : "none");

    if (state.is_empty) {
        if (Rml::Element* e = el("list-empty-title")) {
            e->SetInnerRML(state.empty_title);
            e->SetProperty("color", text_1);
        }
        if (Rml::Element* e = el("list-empty-hint")) {
            e->SetInnerRML(state.empty_hint);
            e->SetProperty("color", text_3);
        }
        if (Rml::Element* e = el("list-empty-icon")) {
            if (!state.empty_icon.empty()) e->SetAttribute("src", state.empty_icon);
            SetImageColor(e, text_3);
        }
        if (Rml::Element* e = el("list-empty-icon-box")) {
            e->SetProperty("background-color", surface);
            e->SetProperty("border-color", border);
        }
    }

    for (int i = 0; i < kListRows; i++) {
        const std::string n = std::to_string(i);
        Rml::Element* row    = el("lrow-" + n);
        Rml::Element* icon   = el("lrow-icon-" + n);
        Rml::Element* title  = el("lrow-title-" + n);
        Rml::Element* detail = el("lrow-detail-" + n);
        Rml::Element* badge  = el("lrow-badge-" + n);
        Rml::Element* chev   = el("lrow-chevron-" + n);
        Rml::Element* track  = el("lrow-track-" + n);
        Rml::Element* fill   = el("lrow-fill-" + n);

        if (!row) continue;

        if (i >= (int)state.rows.size()) {
            row->SetProperty("display", "none");
            continue;
        }

        const EvoListRow& r = state.rows[i];
        bool focused = r.is_focused && !state.rail_focused;

        row->SetProperty("display", "flex");
        row->SetClass("list-row-focused", focused);
        if (focused) {
            row->SetProperty("background-color", surf_sel);
            row->SetProperty("border-color", accent);
            row->SetProperty("border-width", "1.5px");
        } else {
            row->SetProperty("background-color", surface);
            row->SetProperty("border-color", border);
            row->SetProperty("border-width", "1px");
        }

        if (icon) {
            if (!r.icon_path.empty()) icon->SetAttribute("src", r.icon_path);
            SetImageColor(icon, focused ? accent : text_3);
        }
        if (title) {
            title->SetInnerRML(r.title);
            title->SetProperty("color", text_1);
        }
        if (detail) {
            detail->SetProperty("display", r.detail.empty() ? "none" : "block");
            detail->SetInnerRML(r.detail);
            detail->SetProperty("color", text_2);
        }
        if (badge) {
            if (r.badge.empty()) {
                badge->SetProperty("display", "none");
            } else {
                badge->SetProperty("display", "inline-block");
                badge->SetInnerRML(r.badge);
                if (focused) {
                    badge->SetProperty("background-color", accent);
                    badge->SetProperty("border-color", "#ffffff");
                    badge->SetProperty("color", to_hex_rgb(m_theme.bg_bottom));
                } else {
                    badge->SetProperty("background-color", surf_sel);
                    badge->SetProperty("border-color", border);
                    badge->SetProperty("color", accent);
                }
            }
        }
        if (chev) {
            chev->SetProperty("display", r.has_chevron ? "inline-block" : "none");
            SetImageColor(chev, focused ? accent : text_3);
        }
        if (track) {
            track->SetProperty("display", r.progress >= 0 ? "block" : "none");
            track->SetProperty("background-color", border);
        }
        if (fill) {
            fill->SetProperty("width", pct_string(r.progress));
            fill->SetProperty("background-color", accent);
        }
    }

    for (int i = 0; i < 4; i++) {
        const std::string n = std::to_string(i);
        Rml::Element* hint  = el("lhint-" + n);
        Rml::Element* glyph = el("lhint-glyph-" + n);
        Rml::Element* label = el("lhint-label-" + n);

        if (!hint) continue;

        if (i >= (int)state.hints.size()) {
            hint->SetProperty("display", "none");
            continue;
        }
        hint->SetProperty("display", "flex");
        if (glyph && !state.hints[i].glyph_path.empty())
            glyph->SetAttribute("src", state.hints[i].glyph_path);
        if (label) {
            label->SetInnerRML(state.hints[i].label);
            label->SetProperty("color", text_2);
        }
    }

    /* OPTIONS menu: the file browser's operations card, rows filled by the
     * caller. */
    const bool menuOpen = !state.menu.empty();
    if (Rml::Element* e = el("list-menu-scrim"))
        e->SetProperty("display", menuOpen ? "flex" : "none");
    if (menuOpen) {
        if (Rml::Element* e = el("list-menu-eyebrow")) e->SetInnerRML(state.menu_eyebrow);
        if (Rml::Element* e = el("list-menu-title"))   e->SetInnerRML(state.menu_title);
        if (Rml::Element* e = el("list-menu-sub")) {
            e->SetInnerRML(state.menu_sub);
            e->SetProperty("display", state.menu_sub.empty() ? "none" : "block");
        }
        if (Rml::Element* e = el("list-menu-icon")) {
            if (!state.menu_icon.empty()) e->SetAttribute("src", state.menu_icon);
        }
        for (int i = 0; i < kListMenuRows; i++) {
            const std::string n = std::to_string(i);
            Rml::Element* row = el("lmenu-" + n);
            if (!row) continue;
            if (i >= (int)state.menu.size()) {
                row->SetProperty("display", "none");
                continue;
            }
            const EvoListMenuRow& m = state.menu[i];
            row->SetProperty("display", "flex");
            row->SetClass("action-row-focused", i == state.menu_focus);
            row->SetClass("action-row-danger-slot", m.danger);
            if (Rml::Element* e = el("lmenu-label-" + n)) {
                e->SetInnerRML(m.label);
                e->SetClass("act-label-danger", m.danger);
            }
            if (Rml::Element* e = el("lmenu-desc-" + n)) e->SetInnerRML(m.desc);
            if (Rml::Element* e = el("lmenu-icon-" + n)) {
                if (!m.icon_path.empty()) e->SetAttribute("src", m.icon_path);
            }
            if (Rml::Element* e = el("lmenu-iconbox-" + n))
                e->SetClass("act-icon-danger", m.danger);
        }
    }
}

void EvoRmlApp::RenderList(uint32_t* framebuffer, int width, int height) {
    if (!m_initialized || !m_context || !m_list_doc || !framebuffer) return;

    ShowOnlyScreen(m_list_doc);

    if (m_nav_doc) {
        if (m_last_nav.visible) m_nav_doc->Show();
        else                    m_nav_doc->Hide();
    }

    RenderCachedScreen(1, framebuffer, width, height);
}

/*
 * Scrub preview (#32): the frame at the position under the play head.
 *
 * prospero_thumbnail owns the decode - its own thread, its own format context
 * held open between requests, its own 32-entry cache - and coalesces repeated
 * requests, so asking on every rendered frame while the user drags is the
 * intended usage rather than an abuse of it. All this does is ask, and upload
 * whatever has landed.
 *
 * The worker decodes into one static buffer, so the pointer never changes and
 * ArtSource()'s pointer/dimension comparison would call every new preview
 * "unchanged". The serial it hands back rides along as the content tag, which
 * is what makes each new preview a new texture name.
 */
void EvoRmlApp::UpdateScrubPreview(const EvoPlaybackState& state)
{
    Rml::Element* el = m_playback_doc ? m_playback_doc->GetElementById("scrub-thumb")
                                      : nullptr;
    if (!el)
        return;

    if (!state.scrub_active) {
        if (m_scrub_thumb_shown) {
            el->SetProperty("display", "none");
            m_scrub_thumb_shown = false;
            MarkFrameDirty();
        }
        /* Drop the texture so the next scrub cannot flash the previous one
         * before its own first frame has decoded. */
        if (m_scrub_thumb_serial) {
            ArtSource(kScrubArtSlot, nullptr, 0, 0, std::string());
            m_scrub_thumb_serial = 0;
        }
        m_scrub_thumb_baseline = prospero_thumbnail_serial();
        return;
    }

    if (current_media_path[0])
        prospero_thumbnail_request(current_media_path, state.scrub_target, 1);

    const int words = PROSPERO_THUMB_W * PROSPERO_THUMB_H;
    if ((int)m_scrub_thumb_pixels.size() < words)
        m_scrub_thumb_pixels.resize((size_t)words);

    /* Cheap poll first: the worker is decoding in the background and most
     * rendered frames land on the same preview, so skip the 230 KB copy and
     * the texture upload unless it has actually published a new one. */
    const unsigned long long serial = prospero_thumbnail_serial();
    if (serial && serial != m_scrub_thumb_serial &&
        serial != m_scrub_thumb_baseline) {
        const unsigned long long got =
            prospero_thumbnail_snapshot(m_scrub_thumb_pixels.data(), words);
        if (got) {
            m_scrub_thumb_serial = got;
            const std::string src = ArtSource(kScrubArtSlot,
                                              m_scrub_thumb_pixels.data(),
                                              PROSPERO_THUMB_W, PROSPERO_THUMB_H,
                                              evo_fmt("%llu", got));
            if (!src.empty()) {
                el->SetAttribute("src", src);
                MarkFrameDirty();
            }
        }
    }

    /* Only show the box once there is something in it - an empty 320x180 hole
     * under the timecode looks like a bug, a capsule that grows when the
     * preview arrives does not. Set on change only: this runs every rendered
     * frame of a drag, and an unconditional SetProperty would re-dirty layout
     * on all of them. */
    const bool show = m_scrub_thumb_serial != 0;
    if (show != m_scrub_thumb_shown) {
        el->SetProperty("display", show ? "block" : "none");
        m_scrub_thumb_shown = show;
        MarkFrameDirty();
    }
}

void EvoRmlApp::UpdatePlaybackState(const EvoPlaybackState& state) {
    if (!m_initialized || !m_playback_doc) return;

    /*
     * Ahead of the unchanged-state early return below, because the preview is
     * decoded asynchronously: hold the stick still and scrub_target stops
     * moving, so the state compares equal and this function returns - while
     * the frame the user is waiting for is still being decoded and lands a
     * couple of hundred milliseconds later. Gating the upload on the state
     * having changed would drop exactly the frame they asked for.
     */
    UpdateScrubPreview(state);

    /* position_sec ticks every playing frame, so this only actually skips
     * while genuinely paused and idle - the theme-generation gate still
     * catches a theme switch during that pause. */
    if (state == m_last_state && m_theme_generation == m_theme_gen_playback) return;
    m_theme_gen_playback = m_theme_generation;
    m_frame_dirty = true;

    m_last_state = state;

    /* No picture (Twitch's "Audio only"): the card replaces the centre overlay. */
    if (auto* e = m_playback_doc->GetElementById("center-overlay"))
        e->SetProperty("display", state.music_mode ? "none" : "flex");
    if (Rml::Element* mv = m_playback_doc->GetElementById("music-view")) {
        mv->SetProperty("display", state.music_mode ? "flex" : "none");
        if (state.music_mode) {
            if (auto* e = m_playback_doc->GetElementById("music-title"))
                e->SetInnerRML(state.title.empty() ? "Audio only" : state.title);
            if (auto* e = m_playback_doc->GetElementById("music-state"))
                e->SetInnerRML(!state.status.empty() ? state.status
                               : state.paused        ? std::string("PAUSED")
                                                     : std::string("AUDIO ONLY"));
        }
    }

    // 1. Title & Meta
    Rml::Element* el_title = m_playback_doc->GetElementById("media-title");
    if (el_title) {
        el_title->SetInnerRML(state.title.empty() ? "Video Playback" : state.title);
    }

    Rml::Element* el_meta = m_playback_doc->GetElementById("media-meta");
    if (el_meta) {
        if (state.meta.empty()) {
            el_meta->SetProperty("display", "none");
        } else {
            el_meta->SetProperty("display", "block");
            el_meta->SetInnerRML(state.meta);
        }
    }

    // 2. The quality playing, at the top right
    if (Rml::Element* el_q = m_playback_doc->GetElementById("badge-quality")) {
        if (state.quality.empty()) {
            el_q->SetProperty("display", "none");
        } else {
            el_q->SetProperty("display", "inline-block");
            el_q->SetInnerRML(state.quality);
        }
    }
    /* #81: dev FPS pill — independent of the OSD chrome (shows with controls faded). */
    if (Rml::Element* el_fps_pill = m_playback_doc->GetElementById("fps-pill")) {
        el_fps_pill->SetProperty("display", state.debug_overlay ? "block" : "none");
        if (Rml::Element* el_fps_val = m_playback_doc->GetElementById("fps-value"))
            el_fps_val->SetInnerRML(std::to_string(state.fps) + " FPS");
    }

    // 3. Times & Progress
    double cur_pos = state.scrub_active ? state.scrub_target : state.position_sec;
    std::string cur_str = format_time(cur_pos);
    std::string dur_str = format_time(state.duration_sec);
    double remaining = state.duration_sec - cur_pos;
    if (remaining < 0) remaining = 0;
    std::string rem_str = "-" + format_time(remaining);
    std::string time_display = rem_str + " / " + dur_str;

    Rml::Element* el_tcur = m_playback_doc->GetElementById("time-current");
    if (el_tcur) el_tcur->SetInnerRML(cur_str);

    Rml::Element* el_tdur = m_playback_doc->GetElementById("time-duration");
    if (el_tdur) el_tdur->SetInnerRML(time_display);

    double pct = state.percentage * 100.0;
    if (pct < 0.0) pct = 0.0;
    if (pct > 100.0) pct = 100.0;

    Rml::Element* el_fill = m_playback_doc->GetElementById("progress-fill");
    if (el_fill) {
        el_fill->SetProperty("width", evo_fmt("%.1f%%", pct));
        el_fill->SetProperty("background-color", to_hex_rgb(m_theme.accent));
    }

    Rml::Element* el_thumb = m_playback_doc->GetElementById("progress-thumb");
    if (el_thumb) {
        el_thumb->SetProperty("border-color", to_hex_rgb(m_theme.accent));
    }

    // 4. Scrubbing Capsule
    Rml::Element* el_scrub = m_playback_doc->GetElementById("scrub-capsule");
    if (el_scrub) {
        el_scrub->SetProperty("border-color", to_hex_rgb(m_theme.accent));
        if (state.scrub_active) {
            el_scrub->SetProperty("display", "flex");
            Rml::Element* el_stime = m_playback_doc->GetElementById("scrub-time");
            if (el_stime) el_stime->SetInnerRML(format_time(state.scrub_target));
        } else {
            el_scrub->SetProperty("display", "none");
        }
    }

    Rml::Element* el_scrub_lbl = m_playback_doc->GetElementById("scrub-label");
    if (el_scrub_lbl) {
        el_scrub_lbl->SetProperty("color", to_hex_rgb(m_theme.accent));
    }

    // 5. Play/Pause State
    Rml::Element* el_pause = m_playback_doc->GetElementById("pause-badge");
    if (el_pause) {
        el_pause->SetProperty("background-color", to_hex_rgb(m_theme.accent));
        el_pause->SetProperty("border-color", to_hex_rgb(m_theme.border_sel));
        /* The label carries its own colour in the stylesheet, so inheriting
         * from the badge is not enough - set it on the span itself. */
        if (Rml::Element* el_pause_lbl = m_playback_doc->GetElementById("pause-label"))
            el_pause_lbl->SetProperty("color", ink_on(m_theme.accent));
        /* A seek raises the engine's paused flag while it runs: LOADING says
         * what is really going on, PAUSED would be wrong. */
        if (state.paused && !state.scrub_active && !state.loading) {
            el_pause->SetProperty("display", "flex");
        } else {
            el_pause->SetProperty("display", "none");
        }
    }

    if (Rml::Element* el_load = m_playback_doc->GetElementById("loading-badge")) {
        el_load->SetProperty("border-color", to_hex_rgb(m_theme.border_sel));
        el_load->SetProperty("display",
                             ((state.loading || !state.status.empty()) && !state.scrub_active) ? "flex" : "none");
        if (Rml::Element* label = m_playback_doc->GetElementById("loading-label"))
            label->SetInnerRML(state.status.empty() ? std::string("LOADING") : state.status);
    }

    Rml::Element* el_pp_label = m_playback_doc->GetElementById("label-playpause");
    if (el_pp_label) {
        el_pp_label->SetInnerRML(state.paused ? "PLAY" : "PAUSE");
    }

    // 6. The keys. A Twitch stream: R2 quality, R1 chat, Triangle writes to
    // chat and Square makes a clip (logged in). Anything else: Cross, the
    // D-pad (not live), Triangle the aspect.
    auto show = [&](const char* id, bool on) {
        if (Rml::Element* e = m_playback_doc->GetElementById(id))
            e->SetProperty("display", on ? "flex" : "none");
    };
    show("item-playpause", !state.twitch);
    show("item-seek", !state.twitch && !state.is_live);
    show("item-quality", (state.twitch || (state.twitch_video && !state.is_live)) && !state.quality.empty());
    show("item-chat", state.twitch);
    show("item-triangle", (!state.twitch && !state.twitch_video) || state.can_chat);
    show("item-square", state.twitch && state.can_clip);
    show("item-stats", state.can_stats);  /* dev builds only */
    if (Rml::Element* label = m_playback_doc->GetElementById("label-chat"))
        label->SetInnerRML(state.chat_mode == EVO_OSD_CHAT_SIDE      ? "CHAT: NEXT TO THE VIDEO"
                           : state.chat_mode == EVO_OSD_CHAT_OVERLAY ? "CHAT: OVER THE VIDEO"
                                                                     : "CHAT: OFF");
    if (Rml::Element* label = m_playback_doc->GetElementById("label-triangle")) {
        const char* vm = (state.view_mode == 0) ? "FIT" : ((state.view_mode == 1) ? "FILL" : "STRETCH");
        label->SetInnerRML(state.twitch ? std::string("SEND MESSAGE") : std::string("ASPECT: ") + vm);
    }

    /* Live TV and Twitch: there is no timeline to show or seek through. */
    show("progress-container", !state.is_live && !state.twitch);
    show("time-row", !state.is_live && !state.twitch);

    // 7. Stats for Nerds HUD
    Rml::Element* el_stats = m_playback_doc->GetElementById("stats-hud");
    if (el_stats) {
        el_stats->SetProperty("display",
            state.show_stats ? "flex" : "none");
    }
}

/* #81 / #63: the playback diagnostic HUD. Only called while the HUD is visible
 * (OPTIONS on the player), at ~2 Hz, so it writes the elements directly rather
 * than going through the state-diff dance. */
void EvoRmlApp::UpdatePerfHud(const evo_perf_hud_t* h) {
    if (!m_initialized || !m_playback_doc || !h) return;
    m_frame_dirty = true;

    auto set_line = [&](const char* id, const char* text) {
        if (Rml::Element* e = m_playback_doc->GetElementById(id))
            e->SetInnerRML(text && text[0] ? text : "--");
    };
    set_line("stats-video",  h->line_video);
    set_line("stats-audio",  h->line_audio);
    set_line("stats-perf",   h->line_perf);
    set_line("stats-queues", h->line_queues);
    set_line("stats-clocks", h->line_clocks);

    struct GraphSpec { const char* graph_id; const char* val_id; const char* colour;
                       const float* hist; float cur; float peak; const char* unit; };
    char gpu_v[48], ram_v[48], cpu_v[48];
    std::snprintf(gpu_v, sizeof gpu_v, "%.0f%% / %.0f", h->gpu_pct, h->gpu_peak_pct);
    std::snprintf(ram_v, sizeof ram_v, "%.0f / %.0fM", h->ram_mb, h->ram_total_mb);
    std::snprintf(cpu_v, sizeof cpu_v, "%.0f%% / %.0f", h->cpu_pct, h->cpu_peak_pct);
    const GraphSpec specs[3] = {
        { "graph-gpu", "graph-gpu-val", "#00d2ff", h->gpu_hist, h->gpu_pct, h->gpu_peak_pct, gpu_v },
        { "graph-ram", "graph-ram-val", "#ffb020", h->ram_hist, h->ram_mb,  h->ram_peak_mb,  ram_v },
        { "graph-cpu", "graph-cpu-val", "#00ffaa", h->cpu_hist, h->cpu_pct, h->cpu_peak_pct, cpu_v },
    };

    int n = h->hist_len;
    if (n < 0) n = 0;
    for (const GraphSpec& s : specs) {
        Rml::Element* g = m_playback_doc->GetElementById(s.graph_id);
        if (!g) continue;
        /* Lazily create the bar elements once, then only move their heights. */
        while ((int)g->GetNumChildren() < n) {
            Rml::ElementPtr bar = g->GetOwnerDocument()->CreateElement("div");
            bar->SetClass("graph-bar", true);
            bar->SetProperty("background-color", s.colour);
            g->AppendChild(std::move(bar));
        }
        for (int i = 0; i < (int)g->GetNumChildren(); i++) {
            Rml::Element* bar = g->GetChild(i);
            if (i >= n || !s.hist) { bar->SetProperty("height", "0px"); continue; }
            float v = s.hist[i];
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            char hbuf[16];
            std::snprintf(hbuf, sizeof hbuf, "%.0fpx", 2.0f + v * 30.0f);
            bar->SetProperty("height", hbuf);
        }
        if (Rml::Element* v = m_playback_doc->GetElementById(s.val_id))
            v->SetInnerRML(s.unit);
    }
}

void EvoRmlApp::RenderPlaybackOSD(uint32_t* framebuffer, int width, int height) {
    if (!m_initialized || !m_context || !m_playback_doc || !framebuffer) return;

    ShowOnlyScreen(m_playback_doc);
    if (m_nav_doc) m_nav_doc->Hide();   /* nothing over the film but the OSD */

    /* Scroll a long movie title. This document is not surface-cached, so the
     * per-frame re-render this relies on is already happening. */
    MarqueeTick(m_playback_doc->GetElementById("media-title"), true);

    m_render->SetFramebuffer(framebuffer);
    m_render->SetDimensions(width, height);

    /* Overlay over live video — always render straight to the framebuffer. */
    EVO_PROF_CTX_RENDER();
}

void EvoRmlApp::UpdateDialogState(const EvoDialogState& state) {
    if (!m_initialized || !m_dialog_doc) return;
    if (state == m_last_dialog && m_theme_generation == m_theme_gen_dialog) return;
    m_theme_gen_dialog = m_theme_generation;
    m_frame_dirty = true;

    m_last_dialog = state;

    Rml::Element* el_eb = m_dialog_doc->GetElementById("dialog-eyebrow");
    if (el_eb) el_eb->SetInnerRML(state.eyebrow);

    Rml::Element* el_ti = m_dialog_doc->GetElementById("dialog-title");
    if (el_ti) el_ti->SetInnerRML(state.title.empty() ? "Confirmation" : state.title);

    Rml::Element* el_de = m_dialog_doc->GetElementById("dialog-detail");
    if (el_de) el_de->SetInnerRML(state.detail);

    Rml::Element* el_track = m_dialog_doc->GetElementById("dialog-progress-track");
    Rml::Element* el_fill = m_dialog_doc->GetElementById("dialog-progress-fill");
    if (el_track && el_fill) {
        if (state.progress_pct >= 0.0) {
            el_track->SetProperty("display", "block");
            double pct = (state.progress_pct <= 1.0) ? (state.progress_pct * 100.0) : state.progress_pct;
            if (pct < 0.0) pct = 0.0;
            if (pct > 100.0) pct = 100.0;
            el_fill->SetProperty("width", evo_fmt("%.1f%%", pct));
            el_fill->SetProperty("background-color", to_hex_rgb(m_theme.accent));
        } else {
            el_track->SetProperty("display", "none");
        }
    }

    for (int i = 0; i < 3; i++) {
        std::string btn_id = "action-" + std::to_string(i);
        std::string icon_id = "action-icon-" + std::to_string(i);
        std::string label_id = "action-label-" + std::to_string(i);

        Rml::Element* el_btn = m_dialog_doc->GetElementById(btn_id);
        Rml::Element* el_icon = m_dialog_doc->GetElementById(icon_id);
        Rml::Element* el_lbl = m_dialog_doc->GetElementById(label_id);

        if (el_btn) {
            if (i < (int)state.actions.size()) {
                el_btn->SetProperty("display", "flex");
                el_btn->SetClass("btn-primary", state.actions[i].is_primary);
                el_btn->SetClass("btn-secondary", !state.actions[i].is_primary);

                bool focused = (i == state.focused_action);
                el_btn->SetClass("btn-focused", focused);

                if (state.actions[i].is_primary) {
                    el_btn->SetProperty("background-color", to_hex_rgb(m_theme.accent));
                    el_btn->SetProperty("border-color", to_hex_rgb(m_theme.border_sel));
                    el_btn->SetProperty("color", ink_on(m_theme.accent));
                } else {
                    el_btn->SetProperty("background-color", to_hex_rgba(m_theme.surface));
                    el_btn->SetProperty("border-color", to_hex_rgba(m_theme.border));
                    el_btn->SetProperty("color", "#e2e8f0");
                }
                // #65: D-pad focus ring — a bright accent border over whatever
                // the primary/secondary style set.
                if (focused) {
                    el_btn->SetProperty("border-color", to_hex_rgb(m_theme.accent));
                    el_btn->SetProperty("border-width", "3px");
                } else {
                    el_btn->SetProperty("border-width", "2px");
                }

                if (el_icon) el_icon->SetAttribute("src", state.actions[i].icon_path);
                if (el_lbl) el_lbl->SetInnerRML(state.actions[i].label);
            } else {
                el_btn->SetProperty("display", "none");
            }
        }
    }
}

void EvoRmlApp::RenderDialog(uint32_t* framebuffer, int width, int height) {
    if (!m_initialized || !m_context || !m_dialog_doc || !framebuffer) return;

    ShowOnlyScreen(m_dialog_doc);

    m_render->SetFramebuffer(framebuffer);
    m_render->SetDimensions(width, height);

    /* Modal, sometimes over live video — always render straight to the framebuffer. */
    EVO_PROF_CTX_RENDER();
}

void EvoRmlApp::UpdateSettingsState(const EvoSettingsState& state) {
    if (!m_initialized || !m_settings_doc) return;
    if (state == m_last_settings && m_theme_generation == m_theme_gen_settings) return;
    m_theme_gen_settings = m_theme_generation;
    m_frame_dirty = true;

    if (!m_version.empty()) {
        if (Rml::Element* vel = m_settings_doc->GetElementById("footer-version"))
            vel->SetInnerRML(m_version);
    }

    m_last_settings = state;

    Rml::Element* el_ti = m_settings_doc->GetElementById("settings-title");
    if (el_ti) el_ti->SetInnerRML(state.title);

    Rml::Element* el_sub = m_settings_doc->GetElementById("settings-subtitle");
    if (el_sub) el_sub->SetInnerRML(state.subtitle);

    Rml::Element* el_cnt = m_settings_doc->GetElementById("settings-counter");
    if (el_cnt) el_cnt->SetInnerRML(state.counter);

    Rml::Element* el_ind = m_settings_doc->GetElementById("header-indicator");
    if (el_ind) {
        el_ind->SetProperty("background-color", to_hex_rgb(m_theme.accent));
    }
    Rml::Element* el_sicon = m_settings_doc->GetElementById("settings-badge-icon");
    if (el_sicon) {
        SetImageColor(el_sicon, to_hex_rgb(m_theme.accent_alt));
    }

    /* Two-pane sidebar. The section labels are static markup (sb-0 .. sb-2,
     * same order as kSections in SettingsScreen.cpp); only the highlight
     * moves. `sidebar_focused` is set by the index page, where the sidebar IS
     * the page; the section pages light the current row instead. */
    for (int sct = 0; sct < EVO_RMLUI_SETTINGS_SECTIONS; sct++) {
        Rml::Element* el_s = m_settings_doc->GetElementById("sb-" + std::to_string(sct));
        if (!el_s) continue;
        const bool s_active = (sct == state.section_active);
        const bool s_focused = s_active && state.sidebar_focused;
        el_s->SetClass("sb-section-active", s_active && !s_focused);
        el_s->SetClass("sb-section-focused", s_focused);
        /* Paint inline as well as by class. A class change needs a style pass
         * to reach computed values, so on its own the highlight lands one
         * render late - measurable in uiview, where each fixture renders once.
         * The rail block below has always set its colours inline for the same
         * reason; this keeps the two consistent. */
        if (s_focused) {
            el_s->SetProperty("background-color", "#ffcd0026");
            el_s->SetProperty("border-color", to_hex_rgb(m_theme.accent));
        } else if (s_active) {
            el_s->SetProperty("background-color", "#ffffff14");
            el_s->SetProperty("border-color", "#ffffff24");
        } else {
            el_s->SetProperty("background-color", "transparent");
            el_s->SetProperty("border-color", "transparent");
        }
        if (Rml::Element* el_si = m_settings_doc->GetElementById("sb-icon-" + std::to_string(sct))) {
            SetImageColor(el_si, s_focused ? to_hex_rgb(m_theme.accent)
                                           : (s_active ? to_hex_rgb(m_theme.accent_alt)
                                                       : to_hex_rgb(m_theme.text_secondary)));
        }
        if (Rml::Element* el_sl = m_settings_doc->GetElementById("sb-label-" + std::to_string(sct)))
            el_sl->SetProperty("color", s_focused ? to_hex_rgb(m_theme.accent) : "#ffffff");
    }

    for (int r = 0; r < 7; r++) {
        std::string rid = "rail-" + std::to_string(r);
        Rml::Element* el_r = m_settings_doc->GetElementById(rid);
        if (el_r) {
            bool is_active = (r == state.rail_active_idx);
            bool is_focused = is_active && state.rail_focused;
            el_r->SetClass("rail-active", is_active);
            el_r->SetClass("rail-focused", is_focused);

            if (is_focused) {
                el_r->SetProperty("background-color", to_hex_rgb(m_theme.accent));
                el_r->SetProperty("border-color", "#ffffff");
            } else if (is_active) {
                el_r->SetProperty("background-color", to_hex_rgba(m_theme.surface_sel));
                el_r->SetProperty("border-color", to_hex_rgb(m_theme.border_sel));
            } else {
                el_r->SetProperty("background-color", "transparent");
                el_r->SetProperty("border-color", "transparent");
            }
        }
    }

    for (int i = 0; i < EVO_RMLUI_SETTINGS_ROWS; i++) {
        std::string row_id = "row-" + std::to_string(i);
        std::string icon_id = "row-icon-" + std::to_string(i);
        std::string title_id = "row-title-" + std::to_string(i);
        std::string detail_id = "row-detail-" + std::to_string(i);
        std::string badge_id = "row-badge-" + std::to_string(i);
        std::string chev_id = "row-chevron-" + std::to_string(i);
        std::string tog_id = "row-toggle-" + std::to_string(i);
        std::string chk_id = "row-check-" + std::to_string(i);
        std::string ibox_id = "row-iconbox-" + std::to_string(i);

        Rml::Element* el_row = m_settings_doc->GetElementById(row_id);
        Rml::Element* el_icon = m_settings_doc->GetElementById(icon_id);
        Rml::Element* el_title = m_settings_doc->GetElementById(title_id);
        Rml::Element* el_detail = m_settings_doc->GetElementById(detail_id);
        Rml::Element* el_badge = m_settings_doc->GetElementById(badge_id);
        Rml::Element* el_chev = m_settings_doc->GetElementById(chev_id);
        Rml::Element* el_tog = m_settings_doc->GetElementById(tog_id);
        Rml::Element* el_chk = m_settings_doc->GetElementById(chk_id);
        Rml::Element* el_ibox = m_settings_doc->GetElementById(ibox_id);

        if (el_row) {
            if (i < (int)state.rows.size()) {
                el_row->SetProperty("display", "flex");
                bool is_focused = state.rows[i].is_focused && !state.rail_focused;
                el_row->SetClass("row-focused", is_focused);

                if (is_focused) {
                    /* Fill only: the separator hairlines are RCSS-owned so that
                     * .row-focused can swallow the one beneath the highlight. */
                    el_row->SetProperty("background-color", to_hex_rgba(m_theme.surface_sel));
                    if (el_detail) el_detail->SetProperty("color", to_hex_rgb(m_theme.accent));
                    if (el_badge) {
                        el_badge->SetProperty("background-color", "#ffcd0029");
                        el_badge->SetProperty("border-color", to_hex_rgb(m_theme.accent));
                        el_badge->SetProperty("color", to_hex_rgb(m_theme.accent));
                    }
                } else {
                    /* Transparent so the frosted card shows through the row. */
                    el_row->SetProperty("background-color", "transparent");
                    if (el_detail) el_detail->SetProperty("color", to_hex_rgb(m_theme.text_muted));
                    if (el_badge) {
                        el_badge->SetProperty("background-color", "#0a101c99");
                        el_badge->SetProperty("border-color", to_hex_rgba(m_theme.border));
                        el_badge->SetProperty("color", to_hex_rgb(m_theme.text_secondary));
                    }
                }

                if (el_icon) {
                    el_icon->SetAttribute("src", state.rows[i].icon_path.empty() ? "../icons/icon_settings.png" : state.rows[i].icon_path);
                    SetImageColor(el_icon, is_focused
                        ? to_hex_rgb(m_theme.accent)
                        : to_hex_rgb(m_theme.text_secondary));
                }
                if (el_title) el_title->SetInnerRML(state.rows[i].title);
                if (el_detail) el_detail->SetInnerRML(state.rows[i].detail);
                /* A toggle row owns the right-hand slot outright: no badge and
                 * no chevron, so the switch is the only affordance there. */
                const bool is_toggle = (state.rows[i].kind == EVO_RMLUI_ROW_TOGGLE);
                const bool is_option = (state.rows[i].kind == EVO_RMLUI_ROW_OPTION);
                el_row->SetClass("row-option", is_option);
                /* Inline, like the rest of this block: a class-only change lands a
                 * render late. */
                el_row->SetProperty("opacity", state.rows[i].is_disabled ? "0.4" : "1");
                /* Indent and strip the icon inline too - same one-render lag as
                 * the sidebar highlight if this were left to the class alone. */
                el_row->SetProperty("padding-left", is_option ? "76dp" : "18dp");
                if (el_ibox) el_ibox->SetProperty("display", is_option ? "none" : "flex");
                if (el_chk) {
                    const bool checked = is_option && state.rows[i].toggle_on;
                    el_chk->SetProperty("display", checked ? "block" : "none");
                    if (checked) el_chk->SetProperty("background-color", to_hex_rgb(m_theme.accent));
                }
                if (el_tog) {
                    el_tog->SetProperty("display", is_toggle ? "block" : "none");
                    el_tog->SetClass("row-toggle-on", is_toggle && state.rows[i].toggle_on);
                    /* One colour for every switch. This used to paint the
                     * theme accent inline while RCSS painted green, so a switch
                     * was green or purple depending on which won - set both
                     * ends to the same value and always set it inline, since a
                     * class-only change lands a render late. */
                    el_tog->SetProperty("background-color",
                                        state.rows[i].toggle_on ? "#2ecc71" : "#ffffff14");
                    el_tog->SetProperty("border-color",
                                        state.rows[i].toggle_on ? "#2ecc71" : "#ffffff29");
                }
                if (el_badge) {
                    if (is_toggle || is_option || state.rows[i].badge.empty()) {
                        el_badge->SetProperty("display", "none");
                    } else {
                        el_badge->SetProperty("display", "inline-block");
                        el_badge->SetInnerRML(state.rows[i].badge);
                    }
                }
                if (el_chev) {
                    const bool show_chev = state.rows[i].has_chevron && !is_toggle && !is_option &&
                                           !state.rows[i].is_disabled;
                    el_chev->SetProperty("display", show_chev ? "inline-block" : "none");
                    SetImageColor(el_chev, is_focused
                        ? to_hex_rgb(m_theme.accent)
                        : to_hex_rgb(m_theme.text_secondary));
                }
            } else {
                el_row->SetProperty("display", "none");
            }
        }
    }
}

void EvoRmlApp::RenderSettings(uint32_t* framebuffer, int width, int height) {
    if (!m_initialized || !m_context || !m_settings_doc || !framebuffer) return;

    ShowOnlyScreen(m_settings_doc);

    /* Nav rail rendered in the same pass — shown/hidden by UpdateNavState */
    if (m_nav_doc) {
        if (m_last_nav.visible)
            m_nav_doc->Show();
        else
            m_nav_doc->Hide();
    }

    RenderCachedScreen(6, framebuffer, width, height);
}

void EvoRmlApp::UpdateAboutState(const EvoAboutState& state) {
    if (!m_initialized || !m_about_doc) return;
    if (state == m_last_about && m_theme_generation == m_theme_gen_about) return;
    m_theme_gen_about = m_theme_generation;
    m_frame_dirty = true;

    m_last_about = state;

    if (Rml::Element* el = m_about_doc->GetElementById("about-title"))
        el->SetInnerRML(state.app_name);

    if (Rml::Element* el = m_about_doc->GetElementById("about-version-pill")) {
        std::string v = state.version.empty() ? m_version : state.version;
        if (!v.empty() && v[0] != 'v' && v[0] != 'V')
            v = "v" + v;
        el->SetInnerRML(v);
        el->SetProperty("background-color", to_hex_rgb(m_theme.accent));
        el->SetProperty("border-color", to_hex_rgb(m_theme.accent));
        el->SetProperty("color", ink_on(m_theme.accent));
    }

    if (Rml::Element* el = m_about_doc->GetElementById("about-build-tag"))
        el->SetInnerRML(state.build_tag);

    if (Rml::Element* el_ind = m_about_doc->GetElementById("header-indicator"))
        el_ind->SetProperty("background-color", to_hex_rgb(m_theme.accent));

    /* The fixed text: the license notice and the credits, the same lines the
     * kit's About page draws (include/evo_about_text.hpp). */
    namespace about = ::evo::about;
    auto fill = [this](const std::string& id, const std::string& text) {
        if (Rml::Element* el = m_about_doc->GetElementById(id)) el->SetInnerRML(text);
    };
    fill("about-tagline", std::string(about::kBasedOn) + " " + about::kNotAffiliated);
    for (int i = 0; i < about::kLicenseCount; ++i)
        fill("about-license-" + std::to_string(i), about::kLicense[i]);
    fill("about-source", about::kSource);
    for (int i = 0; i < about::kCreditCount; ++i) {
        const about::Credit& c = about::kCredits[i];
        fill("about-credit-" + std::to_string(i),
             c.who[0] ? std::string(c.what) + ", by " + c.who : std::string(c.what));
        fill("about-credit-lic-" + std::to_string(i), c.license);
    }
    fill("about-evo", about::kEvoProject);
}

void EvoRmlApp::RenderAbout(uint32_t* framebuffer, int width, int height) {
    if (!m_initialized || !m_context || !m_about_doc || !framebuffer) return;

    ShowOnlyScreen(m_about_doc);

    /* Nav rail rendered in the same pass — shown/hidden by UpdateNavState */
    if (m_nav_doc) {
        if (m_last_nav.visible)
            m_nav_doc->Show();
        else
            m_nav_doc->Hide();
    }

    RenderCachedScreen(7, framebuffer, width, height);
}

/*
 * Safe-to-close (closed.rml). No state to diff: it is drawn for a couple of
 * seconds and then latched by the parked frame loop, so it is simply themed
 * and rendered each time. Nav rail hidden - there is nowhere left to go.
 */
void EvoRmlApp::RenderClosed(uint32_t* framebuffer, int width, int height) {
    if (!m_initialized || !m_context || !m_closed_doc || !framebuffer) return;

    Rml::ElementDocument* d = m_closed_doc;
    const std::string accent = to_hex_rgb(m_theme.accent);
    auto set = [d](const char* id, const char* prop, const std::string& v) {
        if (Rml::Element* el = d->GetElementById(id)) el->SetProperty(prop, v);
    };

    if (Rml::Element* body = d->GetElementById("closed-body")) {
        body->SetProperty("background-color", to_hex_rgb(m_theme.bg_bottom));
        body->SetProperty("decorator", "vertical-gradient(" + to_hex_rgb(m_theme.bg_top) +
                                       " " + to_hex_rgb(m_theme.bg_bottom) + ")");
        body->SetProperty("color", to_hex_rgb(m_theme.text_primary));
    }
    /* A faint wash, not accent_soft: at accent_soft's alpha the ellipse
     * read as a hard-edged shape behind the card rather than a glow. */
    set("closed-glow", "background-color", accent + "0d");
    set("closed-card", "background-color", to_hex_rgba(m_theme.surface));
    set("closed-card", "border-color", to_hex_rgba(m_theme.border));
    set("closed-power-badge", "background-color", accent);
    set("closed-power-badge", "border-color", to_hex_rgb(m_theme.bg_bottom));
    set("closed-eyebrow", "color", accent);
    set("closed-title", "color", to_hex_rgb(m_theme.text_primary));
    set("closed-accent", "background-color", accent);
    set("closed-message", "color", to_hex_rgb(m_theme.text_secondary));
    set("closed-footer", "color", to_hex_rgb(m_theme.text_muted));
    if (Rml::Element* el = d->GetElementById("closed-power"))
        SetImageColor(el, ink_on(m_theme.accent));
    if (Rml::Element* el = d->GetElementById("closed-footer")) {
        std::string v_suffix;
        if (!m_version.empty()) {
            if (m_version[0] == 'v' || m_version[0] == 'V')
                v_suffix = "  " + m_version;
            else
                v_suffix = "  v" + m_version;
        }
        el->SetInnerRML(upper_ascii(::evo::about::kAppName) + v_suffix);
    }
    /* The app's name, from sce_sys/param.json (include/evo_about_text.hpp). */
    if (Rml::Element* el = d->GetElementById("closed-eyebrow"))
        el->SetInnerRML(upper_ascii(::evo::about::kAppName));
    if (Rml::Element* el = d->GetElementById("closed-app"))
        el->SetInnerRML(::evo::about::kAppName);

    Rml::ElementList steps, nums, keys, strongs;
    d->GetElementsByClassName(steps, "closed-step");
    for (Rml::Element* el : steps) {
        el->SetProperty("background-color", to_hex_rgba(m_theme.surface_sel));
        el->SetProperty("border-color", to_hex_rgba(m_theme.border));
    }
    d->GetElementsByClassName(nums, "closed-step-num");
    for (Rml::Element* el : nums) {
        el->SetProperty("background-color", accent);
        el->SetProperty("color", ink_on(m_theme.accent));
    }
    d->GetElementsByClassName(keys, "closed-key");
    for (Rml::Element* el : keys) {
        el->SetProperty("border-color", to_hex_rgba(m_theme.border));
        el->SetProperty("color", to_hex_rgb(m_theme.text_primary));
    }
    d->GetElementsByClassName(strongs, "closed-strong");
    for (Rml::Element* el : strongs)
        el->SetProperty("color", accent);

    m_frame_dirty = true;
    ShowOnlyScreen(m_closed_doc);
    if (m_nav_doc) m_nav_doc->Hide();

    RenderCachedScreen(21, framebuffer, width, height);
}

/*
 * The Twitch screens' stand-in when the kit could not start
 * (twitch_fallback.rml). Themed, and redrawn only when the focused button or
 * the theme changes. The rail (navbar.rml) shows as on any rail screen.
 */
void EvoRmlApp::RenderTwitchFallback(uint32_t* framebuffer, int width, int height, int focus) {
    if (!m_initialized || !m_context || !m_twitch_fallback_doc || !framebuffer) return;

    Rml::ElementDocument* d = m_twitch_fallback_doc;
    if (focus != m_fallback_focus || m_theme_generation != m_theme_gen_fallback) {
        m_fallback_focus = focus;
        m_theme_gen_fallback = m_theme_generation;
        m_frame_dirty = true;
        auto set = [d](const char* id, const char* prop, const std::string& v) {
            if (Rml::Element* el = d->GetElementById(id)) el->SetProperty(prop, v);
        };
        if (Rml::Element* body = d->GetElementById("fb-body")) {
            body->SetProperty("background-color", to_hex_rgb(m_theme.bg_bottom));
            body->SetProperty("decorator", "vertical-gradient(" + to_hex_rgb(m_theme.bg_top) +
                                           " " + to_hex_rgb(m_theme.bg_bottom) + ")");
        }
        set("fb-app", "color", to_hex_rgb(m_theme.accent_alt));
        if (Rml::Element* el = d->GetElementById("fb-app"))
            el->SetInnerRML(::evo::about::kAppName);
        set("fb-title", "color", to_hex_rgb(m_theme.text_primary));
        set("fb-message", "color", to_hex_rgb(m_theme.text_secondary));
        for (int i = 0; i < 2; ++i) {
            Rml::Element* btn = d->GetElementById(i == 0 ? "fb-btn-0" : "fb-btn-1");
            if (!btn) continue;
            const bool on = i == focus;
            btn->SetClass("fb-btn-focused", on);
            btn->SetProperty("border-color", on ? to_hex_rgb(m_theme.accent) : std::string("transparent"));
        }
    }

    ShowOnlyScreen(m_twitch_fallback_doc);
    if (m_nav_doc) {
        if (m_last_nav.visible)
            m_nav_doc->Show();
        else
            m_nav_doc->Hide();
    }
    RenderCachedScreen(22, framebuffer, width, height);
}

void EvoRmlApp::UpdateNavState(const EvoNavState& state) {
    if (!m_initialized || !m_nav_doc) return;
    if (state == m_last_nav && m_theme_generation == m_theme_gen_nav) return;
    m_theme_gen_nav = m_theme_generation;
    m_frame_dirty = true;
    m_last_nav = state;

    /* FPS pill. Set inline, like every other state in this file that has to be
     * correct on the frame it changes. */
    if (Rml::Element* el_pill = m_nav_doc->GetElementById("nav-fps-pill")) {
        el_pill->SetProperty("display", state.show_fps ? "block" : "none");
        if (state.show_fps) {
            if (Rml::Element* el_v = m_nav_doc->GetElementById("nav-fps-value"))
                el_v->SetInnerRML(std::to_string(state.fps) + " FPS");
        }
    }

    /* ---- elevated-privileges banner ----
     * A condition, not an event: it stays up until the sandbox opens, which is
     * why this is a banner rather than the toast it started as. */
    if (Rml::Element* el_warn = m_nav_doc->GetElementById("nav-privilege-banner"))
        el_warn->SetProperty("display", state.storage_locked ? "flex" : "none");

    /* ---- collapsed icon rail ----
     *
     * The Twitch rail (ScreenManager): Home, Following, Browse, Search and
     * Settings are sections 0..4 and markup slots 0..4. The kit draws its own
     * rail; this one shows only on the RmlUi screens the app falls back to
     * when the kit cannot start, and must offer the same places.
     */
    constexpr int rail_sections = 5;

    /* One entry past the sections: QUIT, markup slot 5 at the foot of the
     * rail. It is never the active section, only ever the cursor. */
    for (int i = 0; i <= rail_sections; i++) {
        const int slot = (i == rail_sections) ? 5 : i;
        std::string item_id  = "nav-item-" + std::to_string(slot);
        std::string bar_id   = "nav-bar-"  + std::to_string(slot);
        std::string icon_id  = "nav-icon-" + std::to_string(slot);

        Rml::Element* el_item = m_nav_doc->GetElementById(item_id);
        Rml::Element* el_bar  = m_nav_doc->GetElementById(bar_id);
        Rml::Element* el_icon = m_nav_doc->GetElementById(icon_id);

        if (!el_item) continue;

        /* QUIT is never the active section, whatever the state says. */
        bool is_active = (i < rail_sections && i == state.active_section);
        bool is_cursor = state.rail_focused && (i == state.cursor_index);

        /* Icon pill */
        el_item->SetClass("rail-item-active",  is_active && !is_cursor);
        el_item->SetClass("rail-item-cursor",   is_cursor);

        if (is_cursor) {
            el_item->SetProperty("background-color", to_hex_rgb(m_theme.accent));
            el_item->SetProperty("border-color", "#ffffff");
            el_item->SetProperty("border-width", "1.5px");
        } else if (is_active) {
            el_item->SetProperty("background-color", to_hex_rgba(m_theme.surface_sel));
            el_item->SetProperty("border-color", to_hex_rgba(m_theme.border));
            el_item->SetProperty("border-width", "1px");
        } else {
            el_item->SetProperty("background-color", "transparent");
            el_item->SetProperty("border-color", "transparent");
            el_item->SetProperty("border-width", "0px");
        }

        /* Icon glyph: the cursor pill fills solid accent, so the glyph has to
         * flip to the darkest theme colour there or it disappears into it -
         * same trick as the settings/list focused badges. */
        if (el_icon) {
            if (is_cursor)
                SetImageColor(el_icon, to_hex_rgb(m_theme.bg_bottom));
            else if (is_active)
                SetImageColor(el_icon, to_hex_rgb(m_theme.accent));
            else
                SetImageColor(el_icon, to_hex_rgb(m_theme.text_secondary));
        }

        /* Accent bar */
        if (el_bar) {
            el_bar->SetClass("rail-accent-bar-visible", is_active && !state.rail_focused);
            if (is_active)
                el_bar->SetProperty("background-color", to_hex_rgb(m_theme.accent));
        }
    }

    /* ---- expanded overlay ---- */
    Rml::Element* scrim    = m_nav_doc->GetElementById("nav-scrim");
    Rml::Element* expanded = m_nav_doc->GetElementById("nav-expanded");
    if (scrim)    scrim->SetProperty("display",    state.rail_focused ? "block" : "none");
    if (expanded) expanded->SetProperty("display", state.rail_focused ? "block" : "none");

    /* Same slots as the collapsed rail above. */
    for (int i = 0; i <= rail_sections; i++) {   /* + QUIT, slot 5 */
        const int eslot = (i == rail_sections) ? 5 : i;
        std::string exp_id  = "nav-exp-"       + std::to_string(eslot);
        std::string lbl_id  = "nav-exp-label-" + std::to_string(eslot);
        std::string icon_id = "nav-exp-icon-"  + std::to_string(eslot);

        Rml::Element* el_exp  = m_nav_doc->GetElementById(exp_id);
        Rml::Element* el_lbl  = m_nav_doc->GetElementById(lbl_id);
        Rml::Element* el_icon = m_nav_doc->GetElementById(icon_id);

        if (!el_exp) continue;

        /* QUIT is never the active section, whatever the state says. */
        bool is_active = (i < rail_sections && i == state.active_section);
        bool is_cursor = (i == state.cursor_index);

        el_exp->SetClass("rail-exp-item-active",  is_active && !is_cursor);
        el_exp->SetClass("rail-exp-item-cursor",   is_cursor);

        if (is_cursor) {
            el_exp->SetProperty("background-color", to_hex_rgb(m_theme.accent));
            el_exp->SetProperty("border-color", "#ffffff");
            el_exp->SetProperty("border-width", "1.5px");
            if (el_lbl) el_lbl->SetProperty("color", "#060b16");
            if (el_icon) SetImageColor(el_icon, to_hex_rgb(m_theme.bg_bottom));
        } else if (is_active) {
            el_exp->SetProperty("background-color", to_hex_rgba(m_theme.surface_sel));
            el_exp->SetProperty("border-color", to_hex_rgba(m_theme.border));
            el_exp->SetProperty("border-width", "1px");
            if (el_lbl) el_lbl->SetProperty("color", to_hex_rgb(m_theme.text_primary));
            if (el_icon) SetImageColor(el_icon, to_hex_rgb(m_theme.accent));
        } else {
            el_exp->SetProperty("background-color", "transparent");
            el_exp->SetProperty("border-color", "transparent");
            el_exp->SetProperty("border-width", "0px");
            if (el_lbl) el_lbl->SetProperty("color", to_hex_rgb(m_theme.text_secondary));
            if (el_icon) SetImageColor(el_icon, to_hex_rgb(m_theme.text_secondary));
        }
    }
}

/* ==========================================================================
 * Toast notifications (#75)
 * ========================================================================== */

void EvoRmlApp::UpdateToastState(const EvoToastState& state) {
    if (!m_initialized || !m_toast_doc) return;

    if (!state.visible) {
        m_toast_doc->Hide();
        return;
    }

    /* Content only actually changes at the start of a toast (evo_toast.c
     * fires a fresh title/message/kind); alpha/slide tick every frame while
     * it's up. Diffing content separately from the per-frame animation
     * values avoids the SetInnerRML/SetAttribute calls (a real relayout)
     * firing every frame for no reason - the animation values below are
     * cheap inline-style pushes, fine to always re-apply. */
    bool content_changed = (state.title != m_toast_last_title ||
                            state.message != m_toast_last_message ||
                            state.kind != m_toast_last_kind);

    if (content_changed) {
        m_toast_last_title = state.title;
        m_toast_last_message = state.message;
        m_toast_last_kind = state.kind;

        Rml::Element* el_card = m_toast_doc->GetElementById("toast-card");
        Rml::Element* el_rail = m_toast_doc->GetElementById("toast-rail");
        Rml::Element* el_icon = m_toast_doc->GetElementById("toast-icon");
        Rml::Element* el_title = m_toast_doc->GetElementById("toast-title");
        Rml::Element* el_msg = m_toast_doc->GetElementById("toast-message");

        /* A toast with only a title or only a message is one line, in the
         * title's type (evo_toast.h). */
        const std::string& head = state.title.empty() ? state.message : state.title;
        const bool two = !state.title.empty() && !state.message.empty();
        if (el_title) el_title->SetInnerRML(head);
        if (el_msg) {
            el_msg->SetInnerRML(two ? state.message : std::string());
            el_msg->SetProperty("display", two ? "block" : "none");
        }

        /* Same icon/colour pairing evo_widget_toast used: info and error
         * share the "about" glyph and differ only by colour, ok borrows the
         * resume glyph, tech gets its own (previously tech rendered
         * identically to info - #75 gives it the muted styling the issue
         * asked for). EvoThemeColors carries no "danger" channel (nothing
         * else in the RmlUi bridge needed one yet), so error uses a fixed
         * red matching the four legacy themes' near-identical danger colours
         * rather than threading a new field through SetTheme for this alone. */
        static const char* kDangerRed = "#ff5c5c";
        std::string accent;
        std::string icon_path;
        switch (state.kind) {
            case 1: /* tech */
                accent = to_hex_rgb(m_theme.text_muted);
                icon_path = "../icons/icon_developer_tools.png";
                break;
            case 2: /* error */
                accent = kDangerRed;
                icon_path = "../icons/icon_about_support.png";
                break;
            case 3: /* ok */
                accent = to_hex_rgb(m_theme.accent_alt);
                icon_path = "../icons/icon_resume.png";
                break;
            default: /* info */
                accent = to_hex_rgb(m_theme.accent);
                icon_path = "../icons/icon_about_support.png";
                break;
        }

        if (el_card) el_card->SetClass("toast-tech", state.kind == 1);
        if (el_rail) el_rail->SetProperty("background-color", accent);
        if (el_icon) {
            el_icon->SetAttribute("src", icon_path);
            SetImageColor(el_icon, accent);
        }
    }

    Rml::Element* el_card = m_toast_doc->GetElementById("toast-card");
    if (el_card) {
        double opacity = state.alpha / 255.0;
        if (opacity < 0.0) opacity = 0.0;
        if (opacity > 1.0) opacity = 1.0;
        el_card->SetProperty("opacity", std::to_string(opacity));
        el_card->SetProperty("transform",
            "translateX(" + std::to_string(state.slide) + "px)");
    }

    m_toast_doc->Show();
}

/*
 * The overlay contexts (toast, keyboard, debug) are created at the display
 * size - 3840x2160 with a 2x dp ratio on a 4K output - but their C callers
 * still pass the 1080p design size (EVO_SCREEN_W/H, a literal 1920x1080).
 * SetDimensions builds the projection from what it is given, so on 4K every
 * overlay was drawn at twice its coordinates: the top-right toast card landed
 * off-screen (no toast was ever visible at 4K), and whatever did fall inside
 * the panel showed up in the wrong place - the dark rectangle in the
 * bottom-right corner. Project with the context's own size; the argument is
 * only a fallback for a context with none.
 */
void EvoRmlApp::SetOverlayDimensions(Rml::Context* ctx, int width, int height) {
    const Rml::Vector2i d = ctx ? ctx->GetDimensions() : Rml::Vector2i(0, 0);
    if (d.x > 0 && d.y > 0) {
        width = d.x;
        height = d.y;
    }
    m_render->SetDimensions(width, height);
}

void EvoRmlApp::RenderToast(uint32_t* framebuffer, int width, int height) {
    if (!m_initialized || !m_toast_context || !m_toast_doc || !framebuffer) return;
    if (!m_toast_doc->IsVisible()) return;

    /* Own context: no other document to hide, and nothing here touches
     * m_frame_dirty / the RenderCachedScreen surface cache for whichever
     * menu screen is underneath - see the comment on EvoToastState. */
    m_render->SetFramebuffer(framebuffer);
    SetOverlayDimensions(m_toast_context, width, height);
    m_toast_context->Update();
    m_render->FrameBegin();
    m_toast_context->Render();
    m_render->FrameEnd();
#if defined(EVO_AGC_DEVICE)
    m_drew = true;
#endif
}

/* ---- #81: virtual keyboard modal ------------------------------------- */

static std::string kb_esc(const char* s) {
    std::string o;
    if (!s) return o;
    for (const char* p = s; *p; ++p) {
        switch (*p) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;";  break;
            case '>': o += "&gt;";  break;
            default:  o += *p;      break;
        }
    }
    return o;
}

void EvoRmlApp::UpdateKeyboard(const evo_keyboard_params_t* p) {
    if (!m_initialized || !m_keyboard_doc || !p) return;

    if (!p->visible) {
        if (m_keyboard_doc->IsVisible()) {
            m_keyboard_doc->Hide();
            m_frame_dirty = true;   /* force the screen underneath to redraw clean */
        }
        m_kb_sig.clear();
        return;
    }

    /* cheap change gate */
    std::string sig;
    sig.reserve(160);
    sig += p->native_only ? "N" : "V";
    sig += p->title ? p->title : "";     sig += '\x1f';
    sig += p->text ? p->text : "";       sig += '\x1f';
    sig += p->mode_label ? p->mode_label : "";
    for (int i = 0; i < 4; i++) { sig += '\x1f'; sig += (p->rows[i] ? p->rows[i] : ""); }
    sig += char('0' + (p->focus_row & 7));
    sig += char('0' + (p->focus_col & 15));
    sig += p->show_caret ? '1' : '0';
    { char b[24]; std::snprintf(b, sizeof b, "|%d/%d", p->len, p->max_len); sig += b; }

    if (!m_keyboard_doc->IsVisible()) m_keyboard_doc->Show();
    if (sig == m_kb_sig) return;
    m_kb_sig = sig;
    m_frame_dirty = true;   /* a keystroke / focus move -> redraw the screen + modal */

    Rml::Element* panel = m_keyboard_doc->GetElementById("kb-panel");
    if (panel) panel->SetProperty("display", p->native_only ? "none" : "flex");
    if (p->native_only) return;

    if (Rml::Element* e = m_keyboard_doc->GetElementById("kb-title"))
        e->SetInnerRML(kb_esc(p->title && p->title[0] ? p->title : "ENTER TEXT"));
    if (Rml::Element* e = m_keyboard_doc->GetElementById("kb-mode"))
        e->SetInnerRML(p->mode_label ? p->mode_label : "");
    if (Rml::Element* e = m_keyboard_doc->GetElementById("kb-text"))
        e->SetInnerRML(kb_esc(p->text));
    if (Rml::Element* e = m_keyboard_doc->GetElementById("kb-caret"))
        e->SetClass("hidden", !p->show_caret);
    if (Rml::Element* e = m_keyboard_doc->GetElementById("kb-count")) {
        char b[24]; std::snprintf(b, sizeof b, "%d / %d", p->len, p->max_len);
        e->SetInnerRML(b);
    }

    for (int r = 0; r < 4; r++) {
        const char* row = p->rows[r] ? p->rows[r] : "";
        int rl = (int)std::strlen(row);
        for (int c = 0; c < 10; c++) {
            char id[8]; std::snprintf(id, sizeof id, "k%d", r * 10 + c);
            Rml::Element* key = m_keyboard_doc->GetElementById(id);
            if (!key) continue;
            char ch[2] = { c < rl ? row[c] : ' ', 0 };
            key->SetInnerRML(kb_esc(ch));
            key->SetClass("focused", p->focus_row == r && p->focus_col == c);
        }
    }
    for (int a = 0; a < 6; a++) {
        char id[8]; std::snprintf(id, sizeof id, "a%d", a);
        Rml::Element* act = m_keyboard_doc->GetElementById(id);
        if (!act) continue;
        if (p->action_labels[a]) act->SetInnerRML(p->action_labels[a]);
        act->SetClass("focused", p->focus_row == 4 && p->focus_col == a);
    }
}

void EvoRmlApp::RenderKeyboard(uint32_t* framebuffer, int width, int height) {
    if (!m_initialized || !m_keyboard_context || !m_keyboard_doc || !framebuffer) return;
    if (!m_keyboard_doc->IsVisible()) return;
    m_render->SetFramebuffer(framebuffer);
    SetOverlayDimensions(m_keyboard_context, width, height);
    m_keyboard_context->Update();
    m_render->FrameBegin();
    m_keyboard_context->Render();
    m_render->FrameEnd();
#if defined(EVO_AGC_DEVICE)
    m_drew = true;
#endif
}

void EvoRmlApp::UpdateDebugOverlay(int fps, bool visible) {
    if (!m_initialized || !m_debug_doc) return;
    m_debug_visible = visible;
    if (!visible) {
        if (m_debug_doc->IsVisible()) m_debug_doc->Hide();
        m_debug_last_fps = -1;
        return;
    }
    if (!m_debug_doc->IsVisible()) m_debug_doc->Show();
    if (fps != m_debug_last_fps) {
        m_debug_last_fps = fps;
        if (Rml::Element* e = m_debug_doc->GetElementById("debug-fps"))
            e->SetInnerRML(std::to_string(fps) + " FPS");
    }
}

void EvoRmlApp::RenderDebugOverlay(uint32_t* framebuffer, int width, int height) {
    if (!m_initialized || !m_debug_context || !m_debug_doc || !framebuffer) return;
    if (!m_debug_doc->IsVisible()) return;
    m_render->SetFramebuffer(framebuffer);
    SetOverlayDimensions(m_debug_context, width, height);
    m_debug_context->Update();
    m_render->FrameBegin();
    m_debug_context->Render();
    m_render->FrameEnd();
#if defined(EVO_AGC_DEVICE)
    m_drew = true;
#endif
}

