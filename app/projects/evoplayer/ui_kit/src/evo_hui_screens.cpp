/*
 * evo_hui_screens.cpp - EVO's screens on ps5-homebrew-ui. See the header.
 *
 * The look follows the kit's Aurora Shelf design (third_party/ps5-homebrew-ui
 * src/concepts/aurora.cpp): one focus ring that glides between whatever has
 * the cursor, and entrances that stagger in rather than pop.
 */
#include "evo_hui_screens.hpp"
#include "evo_about_text.hpp"
#include "evo_hui_twitch.hpp"

#include "ui/glyphs.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "../../stb_image.h"
#pragma clang diagnostic pop

namespace evo::kit
{

using ::hui::gfx::Align;
using ::hui::gfx::Color;
using ::hui::gfx::DrawList;
using ::hui::gfx::Rect;
namespace tween = ::hui::tween;
namespace ui = ::hui::ui;

namespace
{

const Color kWhite = Color::rgb(0xffffff);
const Color kInk = Color::rgb(0x0b0d16);

/* EVO theme words are 0xAABBGGRR. */
Color from_evo(std::uint32_t c, bool use_alpha)
{
    const float a = use_alpha ? static_cast<float>((c >> 24) & 0xff) / 255.0f : 1.0f;
    return {static_cast<float>(c & 0xff) / 255.0f, static_cast<float>((c >> 8) & 0xff) / 255.0f,
            static_cast<float>((c >> 16) & 0xff) / 255.0f, a};
}

/* A one-line string cut to width with an ellipsis. */
std::string fit(const ui::FontRef &font, const std::string &text, float size, float width)
{
    return font.font->fit(text, size, width);
}

} // namespace

/* ---------------------------------------------------------------- textures */

TextureCache::~TextureCache()
{
    clear();
}

void TextureCache::clear()
{
    for (auto &kv : entries_)
        host_.release_texture(kv.second.handle);
    entries_.clear();
    for (auto &kv : immutable_)
        host_.release_texture(kv.second.handle);
    immutable_.clear();
}

std::uint32_t TextureCache::pixels_immutable(std::uint64_t id, const std::uint32_t *data, int width, int height)
{
    if (!data || width <= 0 || height <= 0)
        return 0;
    auto it = immutable_.find(id);
    if (it != immutable_.end())
    {
        it->second.idle_frames = 0;
        if (!retry_due(it->second))
            return it->second.handle;
    }
    Entry &e = immutable_[id];
    e.idle_frames = 0;
    e.handle = host_.create_texture(width, height, reinterpret_cast<const std::uint8_t *>(data));
    /* No room on the GPU: the card keeps its placeholder, and the upload is
     * tried again in a while rather than never. */
    e.retry_frames = e.handle ? 0 : kRetryFrames;
    return e.handle;
}

std::uint32_t TextureCache::pixels(const std::uint32_t *data, int width, int height,
                                   std::uint64_t tag)
{
    if (!data || width <= 0 || height <= 0)
        return 0;
    /* The cover service decodes into its buffers after handing them out, so
     * the same pointer first holds black and later the picture. A sparse hash
     * of the pixels goes into the key: new content, new upload. */
    const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    std::uint64_t sample = 1469598103934665603ull;
    for (std::size_t i = 0; i < count; i += 61)
        sample = (sample ^ data[i]) * 1099511628211ull;
    char key[128];
    std::snprintf(key, sizeof(key), "px:%p:%dx%d:%llx:%llx", static_cast<const void *>(data), width,
                  height, static_cast<unsigned long long>(tag),
                  static_cast<unsigned long long>(sample));
    auto it = entries_.find(key);
    if (it != entries_.end())
    {
        it->second.idle_frames = 0;
        if (!retry_due(it->second))
            return it->second.handle;
    }
    Entry &e = entries_[key];
    e.idle_frames = 0;
    e.handle = host_.create_texture(width, height, reinterpret_cast<const std::uint8_t *>(data));
    e.retry_frames = e.handle ? 0 : kRetryFrames;
    return e.handle;
}

std::uint32_t TextureCache::icon(const std::string &path)
{
    if (path.empty())
        return 0;
    const std::string key = "icon:" + path;
    auto it = entries_.find(key);
    if (it != entries_.end())
    {
        it->second.idle_frames = 0;
        if (!retry_due(it->second))
            return it->second.handle;
    }
    Entry e;
    std::string bytes;
    if (host_.read_asset(path, &bytes))
    {
        int w = 0, h = 0, n = 0;
        unsigned char *rgba =
            stbi_load_from_memory(reinterpret_cast<const unsigned char *>(bytes.data()),
                                  static_cast<int>(bytes.size()), &w, &h, &n, 4);
        if (rgba)
        {
            e.handle = host_.create_texture(w, h, rgba);
            stbi_image_free(rgba);
            /* Decoded, but no room on the GPU: try again in a while. */
            e.retry_frames = e.handle ? 0 : kRetryFrames;
        }
    }
    /* A failed load is cached too (handle 0), so a missing icon is not
     * re-read every frame. */
    entries_[key] = e;
    return e.handle;
}

void TextureCache::tick()
{
    /* Failed uploads count down to their next try (retry_due at 1). */
    for (auto &kv : entries_)
        if (kv.second.handle == 0 && kv.second.retry_frames > 1)
            --kv.second.retry_frames;
    for (auto &kv : immutable_)
        if (kv.second.handle == 0 && kv.second.retry_frames > 1)
            --kv.second.retry_frames;
    /* Icons stay; covers that left the screen go after about two seconds. */
    for (auto it = entries_.begin(); it != entries_.end();)
    {
        if (it->first.compare(0, 3, "px:") == 0 && ++it->second.idle_frames > 120)
        {
            host_.release_texture(it->second.handle);
            it = entries_.erase(it);
        }
        else
        {
            ++it;
        }
    }
    for (auto it = immutable_.begin(); it != immutable_.end();)
    {
        if (++it->second.idle_frames > 120)
        {
            host_.release_texture(it->second.handle);
            it = immutable_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

/* ----------------------------------------------------------------- palette */

Palette Palette::from(const evo_rmlui_theme_t &t)
{
    Palette p;
    p.bg_top = from_evo(t.bg_top, false);
    p.bg_bottom = from_evo(t.bg_bottom, false);
    p.surface = from_evo(t.surface, true);
    p.surface_sel = from_evo(t.surface_sel, true);
    p.border = from_evo(t.border, true);
    p.accent = from_evo(t.accent, false);
    p.accent_soft = from_evo(t.accent_soft, false);
    p.accent_alt = from_evo(t.accent_alt, false);
    p.text = from_evo(t.text_primary, false);
    p.text_muted = from_evo(t.text_secondary, false);
    p.text_faint = from_evo(t.text_muted, false);
    return p;
}

/* ---------------------------------------------------------------- backdrop */

void draw_backdrop(DrawList &list, const Context &ctx, std::uint32_t picture, float picture_alpha,
                   Rect hole)
{
    const Palette &p = ctx.palette;
    const Rect screen{0, 0, ::hui::gfx::kVirtualWidth, ::hui::gfx::kVirtualHeight};
    const bool cut = hole.w > 1.0f && hole.h > 1.0f;
    if (!cut)
        list.gradient_rect(screen, 0, p.bg_top, p.bg_bottom);
    else
    {
        /* Four pieces around the hole, so a video quad under the page shows
         * through. The gradient keeps the same colours it would have had. */
        const float H = screen.h;
        const float W = screen.w;
        auto col = [&](float y) {
            const float t = std::clamp(y / H, 0.0f, 1.0f);
            return ::hui::gfx::mix(p.bg_top, p.bg_bottom, t);
        };
        const float y0 = std::clamp(hole.y, 0.0f, H);
        const float y1 = std::clamp(hole.y + hole.h, 0.0f, H);
        const float x0 = std::clamp(hole.x, 0.0f, W);
        const float x1 = std::clamp(hole.x + hole.w, 0.0f, W);
        if (y0 > 0.0f)
            list.gradient_rect({0, 0, W, y0}, 0, col(0), col(y0));
        if (y1 < H)
            list.gradient_rect({0, y1, W, H - y1}, 0, col(y1), col(H));
        if (y1 > y0)
        {
            if (x0 > 0.0f)
                list.gradient_rect({0, y0, x0, y1 - y0}, 0, col(y0), col(y1));
            if (x1 < W)
                list.gradient_rect({x1, y0, W - x1, y1 - y0}, 0, col(y0), col(y1));
        }
    }

    /* The picture, washed in from the right: the left side stays dark for
     * the text that sits on it. A hole means that picture would cover the
     * video, so it stays off. */
    if (!cut && picture && picture_alpha > 0.0f)
    {
        list.image(picture, screen, ::hui::gfx::kFullUv, kWhite.with_alpha(0.55f * picture_alpha));
        list.gradient_rect_h(screen, 0, p.bg_bottom.with_alpha(0.97f), p.bg_bottom.with_alpha(0.35f));
        list.gradient_rect({0, 540, screen.w, 540}, 0, p.bg_bottom.with_alpha(0.0f),
                           p.bg_bottom.with_alpha(0.92f));
    }

    /* Aurora: three large soft lights that drift on slow, unrelated periods. */
    const float t = ctx.time;
    struct Light
    {
        float x, y, r, period, phase;
        Color c;
    };
    const Light lights[] = {
        {1500, 160, 360, 23.0f, 0.0f, p.accent.with_alpha(0.16f)},
        {420, 900, 420, 31.0f, 2.1f, p.accent_alt.with_alpha(0.12f)},
        {1200, 980, 300, 19.0f, 4.2f, p.accent_soft.with_alpha(0.14f)},
    };
    for (const Light &l : lights)
    {
        const float a = 6.2831853f * t / l.period + l.phase;
        const float x = l.x + 120.0f * std::sin(a);
        const float y = l.y + 70.0f * std::cos(a * 0.7f);
        if (cut)
        {
            const float nx = std::clamp(x, hole.x, hole.x + hole.w);
            const float ny = std::clamp(y, hole.y, hole.y + hole.h);
            const float dx = x - nx, dy = y - ny;
            if (dx * dx + dy * dy < l.r * l.r)
                continue;
        }
        list.shadow({x - l.r, y - l.r, 2 * l.r, 2 * l.r}, l.r, l.r * 0.9f, l.c);
    }
}

/* ----------------------------------------------------------------- the rail */

namespace
{

/* Icons the bundle has no PNG for, drawn from shapes in the same line style. */
enum class RailGlyph
{
    png,
    grid,   /* Browse: four rounded squares */
    search, /* a magnifier */
    heart,  /* Following, as on Twitch */
};

struct RailItem
{
    const char *label;
    const char *icon;
    RailGlyph glyph;
};

/* ScreenManager's section order (the Twitch rail), then Quit. */
int rail_items(RailItem *out)
{
    int n = 0;
    out[n++] = {"Home", "../icons/icon_home.png", RailGlyph::png};
    out[n++] = {"Following", nullptr, RailGlyph::heart};
    out[n++] = {"Browse", nullptr, RailGlyph::grid};
    out[n++] = {"Search", nullptr, RailGlyph::search};
    out[n++] = {"Settings", "../icons/icon_settings.png", RailGlyph::png};
    out[n++] = {"Quit", "../icons/icon_power.png", RailGlyph::png};
    return n;
}

/* A rail icon in the 32 x 32 box `r`: the PNG tinted, or the drawn glyph. */
void draw_rail_icon(DrawList &list, const Context &ctx, const RailItem &item, const Rect &r, Color ink)
{
    constexpr float kStroke = 2.4f; /* the PNG icons' line weight at 32 px */
    switch (item.glyph)
    {
    case RailGlyph::grid:
    {
        const Rect box = r.inset(2.0f); /* the PNGs leave the same margin */
        const float gap = 4.0f, cell = (box.w - gap) * 0.5f;
        for (int i = 0; i < 4; ++i)
        {
            const Rect c{box.x + static_cast<float>(i % 2) * (cell + gap),
                         box.y + static_cast<float>(i / 2) * (cell + gap), cell, cell};
            list.bordered_rect(c, 3.5f, Color::rgb(0x000000, 0.0f), kStroke, ink);
        }
        break;
    }
    case RailGlyph::search:
    {
        const float cx = r.x + r.w * 0.42f, cy = r.y + r.h * 0.42f, rad = r.w * 0.33f;
        list.ring(cx, cy, rad, kStroke, ink);
        const float d = rad * 0.7071f;
        list.line(cx + d + 1.0f, cy + d + 1.0f, r.x + r.w - 3.0f, r.y + r.h - 3.0f, kStroke, ink);
        break;
    }
    case RailGlyph::heart:
    {
        /* A square on its corner with a half circle on each upper side: the
         * outline runs up the left side from the tip, over both lobes and
         * back down. Angles go clockwise from 12 o'clock; arc() takes the
         * outer radius, so the stroke is centred on the lobe's circle. */
        const float h = 10.8f;                       /* the square's half diagonal */
        const float lobe = h * 0.7071f;              /* half its side */
        const float cx = r.x + r.w * 0.5f;
        const float cy = r.y + r.h * 0.5f + 0.1035f * h + 0.6f; /* the outline's middle at the box's */
        const float half_pi = 1.5707963f;
        list.arc(cx - h * 0.5f, cy - h * 0.5f, lobe + kStroke * 0.5f, kStroke, 2.5f * half_pi, 2.0f * half_pi, ink);
        list.arc(cx + h * 0.5f, cy - h * 0.5f, lobe + kStroke * 0.5f, kStroke, 3.5f * half_pi, 2.0f * half_pi, ink);
        list.line(cx - h, cy, cx, cy + h, kStroke, ink);
        list.line(cx + h, cy, cx, cy + h, kStroke, ink);
        break;
    }
    case RailGlyph::png:
        if (const std::uint32_t icon = ctx.textures.icon(item.icon))
            list.image(icon, r, ::hui::gfx::kFullUv, ink);
        break;
    }
}

constexpr float kRailTop = 300.0f;
constexpr float kRailPitch = 88.0f;
constexpr float kRailExpanded = 360.0f;

float rail_item_y(int index, int count)
{
    /* Quit sits apart at the foot of the rail. */
    if (index == count - 1)
        return 1080.0f - 150.0f;
    return kRailTop + static_cast<float>(index) * kRailPitch;
}

} // namespace

void NavRail::update(float dt)
{
    expand_.target = params_.rail_focused ? 1.0f : 0.0f;
    expand_.update(dt, 16.0f);
    RailItem items[8];
    const int count = rail_items(items);
    const int at = params_.rail_focused ? params_.cursor_index : params_.active_section;
    const int index = std::clamp(at, 0, count - 1);
    const float width = tween::lerp(72.0f, kRailExpanded - 56.0f, expand_.value);
    const Rect target{28.0f, rail_item_y(index, count), width, 72.0f};
    if (!cursor_snapped_)
    {
        cursor_.snap(target);
        cursor_snapped_ = true;
    }
    cursor_.target(target);
    cursor_.update(dt, 22.0f);
}

void NavRail::draw(DrawList &list, const Context &ctx) const
{
    if (!params_.visible)
        return;
    const Palette &p = ctx.palette;
    const ui::Fonts &fonts = ctx.fonts;
    const float e = expand_.value;
    RailItem items[8];
    const int count = rail_items(items);

    /* Scrim over the content while the rail is open. */
    if (e > 0.01f)
        list.rounded_rect({0, 0, 1920, 1080}, 0, Color::rgb(0x000000, 0.5f * e));

    const float width = tween::lerp(kWidth, kRailExpanded, e);
    const Rect panel{0, 0, width, 1080};
    list.gradient_rect_h(panel, 0, p.bg_bottom.with_alpha(0.94f + 0.05f * e),
                         p.bg_bottom.with_alpha(0.7f + 0.27f * e));
    list.rounded_rect({width - 1.5f, 0, 1.5f, 1080}, 0, kWhite.with_alpha(0.07f));

    /* The mark: the app's "twi" tile, and its name beside it while the rail
     * is open. */
    draw_app_mark(list, ctx, 32, 62, 64);
    if (e > 0.01f)
    {
        list.push_opacity(e);
        ui::text(list, fonts.display, ::evo::about::kAppName, 112, 106, 34, kWhite);
        list.pop_opacity();
    }

    /* The highlight: a pill that glides to the cursor (rail open) or rests on
     * the active section (rail closed). */
    const Rect pill = cursor_.value();
    if (params_.rail_focused)
    {
        list.glow(pill, 36, 18, p.accent.with_alpha(0.45f));
        list.rounded_rect(pill, 36, p.accent);
    }
    else
    {
        list.rounded_rect(pill, 36, kWhite.with_alpha(0.10f));
    }

    for (int i = 0; i < count; ++i)
    {
        const float y = rail_item_y(i, count);
        const bool cursor = params_.rail_focused && i == params_.cursor_index;
        const bool active = i == params_.active_section;
        const Color ink = cursor ? kInk : (active ? kWhite : kWhite.with_alpha(0.62f));
        draw_rail_icon(list, ctx, items[i], {48, y + 20, 32, 32}, ink);
        if (!params_.rail_focused && active)
            list.rounded_rect({8, y + 22, 4, 28}, 2, p.accent);
        if (e > 0.01f)
        {
            list.push_opacity(tween::clamp01(e * 1.6f - 0.4f));
            ui::text(list, cursor ? fonts.semibold : fonts.regular, items[i].label, 104, y + 46, 26,
                     ink);
            list.pop_opacity();
        }
    }

    if (params_.show_fps)
    {
        char fps[24];
        std::snprintf(fps, sizeof(fps), "%d FPS", params_.fps);
        const Rect chip{1920 - 180, 1080 - 64, 140, 40};
        list.rounded_rect(chip, 20, Color::rgb(0x000000, 0.55f));
        ui::text(list, fonts.mono, fps, chip.cx(), chip.y + 28, 20, p.accent, Align::center);
    }
}

/* -------------------------------------------------------------------- list */

namespace
{

constexpr float kLeft = NavRail::kWidth + 64.0f; /* content's left edge */
constexpr float kListTop = 220.0f;
constexpr float kListRowH = 76.0f;
constexpr float kListGap = 8.0f;
constexpr float kListRight = 1824.0f;
constexpr float kMenuW = 640.0f;
constexpr float kMenuItemH = 64.0f;
const Color kDanger = Color::rgb(0xff5a5f);

/* The RmlUi documents name a hint by its glyph file. */
ui::Button button_for(const std::string &glyph)
{
    struct Map
    {
        const char *name;
        ui::Button button;
    };
    static const Map map[] = {{"btn_cross", ui::Button::cross},     {"btn_circle", ui::Button::circle},
                              {"btn_square", ui::Button::square},   {"btn_triangle", ui::Button::triangle},
                              {"btn_dpad", ui::Button::dpad},       {"btn_lstick", ui::Button::left_stick},
                              {"btn_options", ui::Button::options}, {"btn_touchpad", ui::Button::touchpad}};
    for (const Map &m : map)
        if (glyph.find(m.name) != std::string::npos)
            return m.button;
    return ui::Button::none;
}

/* "SELECT" -> "Select": the kit's hints are sentence case. */
std::string title_case(const std::string &s)
{
    std::string out = s;
    bool start = true;
    for (char &c : out)
    {
        if (c >= 'A' && c <= 'Z' && !start)
            c = static_cast<char>(c - 'A' + 'a');
        start = (c == ' ');
    }
    return out;
}

} // namespace

void ListScreen::set(const evo_rmlui_list_params_t &p)
{
    auto str = [](const char *s) { return std::string(s ? s : ""); };
    if (str(p.title) != title_)
    {
        age_ = 0.0f; /* a different list: run the entrance again */
        ring_snapped_ = false;
    }
    title_ = str(p.title);
    subtitle_ = str(p.subtitle);
    total_ = p.total_count;
    cursor_ = p.cursor_index;
    rows_.clear();
    for (int i = 0; i < p.row_count && i < EVO_RMLUI_LIST_ROWS; ++i)
    {
        Row r;
        r.title = str(p.rows[i].title);
        r.detail = str(p.rows[i].detail);
        r.icon = str(p.rows[i].icon_path);
        r.badge = str(p.rows[i].badge);
        r.progress = p.rows[i].progress;
        r.chevron = p.rows[i].has_chevron != 0;
        r.focused = p.rows[i].is_focused != 0;
        r.checked = p.rows[i].is_checked != 0;
        rows_.push_back(r);
    }
    empty_ = p.is_empty != 0;
    empty_title_ = str(p.empty_title);
    empty_hint_ = str(p.empty_hint);
    empty_icon_ = str(p.empty_icon);
    hints_.clear();
    for (int i = 0; i < p.hint_count && i < 4; ++i)
    {
        const ui::Button b = button_for(str(p.hints[i].glyph_path));
        if (b != ui::Button::none)
            hints_.emplace_back(static_cast<int>(b), title_case(str(p.hints[i].label)));
    }
    if (menu_.empty() && p.menu_count > 0)
        menu_ring_snapped_ = false;
    menu_.clear();
    for (int i = 0; i < p.menu_count && i < EVO_RMLUI_LIST_MENU_ROWS; ++i)
    {
        MenuItem m;
        m.label = str(p.menu[i].label);
        m.desc = str(p.menu[i].desc);
        m.icon = str(p.menu[i].icon_path);
        m.danger = p.menu[i].danger != 0;
        menu_.push_back(m);
    }
    menu_focus_ = p.menu_focus;
    menu_eyebrow_ = str(p.menu_eyebrow);
    menu_title_ = str(p.menu_title);
    menu_sub_ = str(p.menu_sub);
    menu_icon_ = str(p.menu_icon);
}

void ListScreen::enter()
{
    age_ = 0.0f;
    ring_snapped_ = false;
    menu_open_.snap(menu_.empty() ? 0.0f : 1.0f);
}

Rect ListScreen::row_rect(int index) const
{
    return {kLeft, kListTop + static_cast<float>(index) * (kListRowH + kListGap), kListRight - kLeft,
            kListRowH};
}

Rect ListScreen::menu_item_rect(int index) const
{
    /* Card centred on the screen; items under its 180 px header. */
    const float h = 180.0f + static_cast<float>(menu_.size()) * (kMenuItemH + 6.0f) + 24.0f;
    const float top = 540.0f - h * 0.5f;
    return {960.0f - kMenuW * 0.5f + 24.0f, top + 180.0f + static_cast<float>(index) * (kMenuItemH + 6.0f),
            kMenuW - 48.0f, kMenuItemH};
}

void ListScreen::update(float dt)
{
    age_ += dt;
    int focused = -1;
    for (std::size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i].focused)
            focused = static_cast<int>(i);
    if (focused >= 0)
    {
        const Rect target = row_rect(focused);
        if (!ring_snapped_)
        {
            ring_.snap(target);
            ring_snapped_ = true;
        }
        ring_.target(target);
    }
    ring_.update(dt, 22.0f);
    has_focus_.target = focused >= 0 ? 1.0f : 0.0f;
    has_focus_.update(dt, 12.0f);

    menu_open_.target = menu_.empty() ? 0.0f : 1.0f;
    menu_open_.update(dt, 18.0f);
    if (!menu_.empty())
    {
        const Rect target = menu_item_rect(std::clamp(menu_focus_, 0, static_cast<int>(menu_.size()) - 1));
        if (!menu_ring_snapped_)
        {
            menu_ring_.snap(target);
            menu_ring_snapped_ = true;
        }
        menu_ring_.target(target);
        menu_ring_.update(dt, 24.0f);
    }
}

void ListScreen::draw(DrawList &list, const Context &ctx) const
{
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    draw_backdrop(list, ctx);

    /* Header. */
    {
        const float in = tween::stagger(age_, 0, 0.05f, 0.45f);
        list.push_opacity(in);
        const float dy = 16.0f * (1.0f - in);
        ui::text(list, fonts.display, fit(fonts.display, title_, 52, 1100), kLeft - 2, 140 + dy, 52,
                 kWhite);
        /* The line under the title as on the Twitch pages: a sentence, not
         * an eyebrow. A list that fits needs no count; a longer one says
         * where the cursor is, as Home's rows do ("3 of 23"). */
        if (!subtitle_.empty())
            ui::text(list, fonts.regular, subtitle_, kLeft, 184 + dy, 26, p.text_muted);
        if (total_ > static_cast<int>(rows_.size()) && cursor_ >= 0)
        {
            char counter[32];
            std::snprintf(counter, sizeof(counter), "%d of %d", cursor_ + 1, total_);
            ui::text(list, fonts.regular, counter, kListRight, 140 + dy, 22, p.text_faint, Align::right);
        }
        list.pop_opacity();
    }

    if (empty_)
    {
        const float in = tween::stagger(age_, 1, 0.08f, 0.6f);
        list.push_opacity(in);
        const float cx = (kLeft + kListRight) * 0.5f;
        const std::uint32_t icon = ctx.textures.icon(empty_icon_);
        if (icon)
            list.image(icon, {cx - 48, 380, 96, 96}, ::hui::gfx::kFullUv, kWhite.with_alpha(0.4f));
        ui::text(list, fonts.display, empty_title_, cx, 560, 40, kWhite, Align::center);
        ui::paragraph(list, fonts.regular, empty_hint_, cx, 610, 24, 900, 34, p.text_muted, 3,
                      Align::center);
        list.pop_opacity();
    }
    else
    {
        /* The highlight's light goes under the rows. */
        if (has_focus_.value > 0.01f)
        {
            list.push_opacity(has_focus_.value);
            list.glow(ring_.value(), 20, 18, p.accent.with_alpha(0.3f));
            list.pop_opacity();
        }
        for (std::size_t i = 0; i < rows_.size(); ++i)
        {
            const Row &row = rows_[i];
            const float in = tween::stagger(age_, static_cast<int>(i) + 1, 0.03f, 0.4f);
            list.push_opacity(in);
            Rect r = row_rect(static_cast<int>(i));
            r.y += 24.0f * (1.0f - tween::cubic_out(in));
            list.rounded_rect(r, 20, row.focused ? kWhite.with_alpha(0.14f) : p.surface);
            const Rect well{r.x + 12, r.y + 10, 56, 56};
            list.rounded_rect(well, 16, row.focused ? p.accent : kWhite.with_alpha(0.08f));
            const std::uint32_t icon = ctx.textures.icon(row.icon);
            if (icon)
                list.image(icon, well.inset(14), ::hui::gfx::kFullUv, row.focused ? kInk : kWhite);

            float right = r.x + r.w - 24;
            if (row.checked)
            {
                /* A tick, the short stroke then the long one, as the kit's
                 * check box draws it; white on the focused row's accent. */
                const float s = 36.0f, x = right - s, y = r.cy() - s * 0.5f;
                const Color ink = row.focused ? kWhite : p.accent;
                list.line(x + s * 0.16f, y + s * 0.52f, x + s * 0.40f, y + s * 0.76f, 4.5f, ink);
                list.line(x + s * 0.40f, y + s * 0.76f, x + s * 0.86f, y + s * 0.26f, 4.5f, ink);
                right -= s + 20;
            }
            if (row.chevron)
            {
                const std::uint32_t chev = ctx.textures.icon("../icons/icon_chevron.png");
                if (chev)
                    list.image(chev, {right - 24, r.cy() - 12, 24, 24}, ::hui::gfx::kFullUv,
                               kWhite.with_alpha(row.focused ? 0.9f : 0.45f));
                right -= 44;
            }
            if (!row.badge.empty())
            {
                const float w = fonts.semibold.measure(row.badge, 16, 1.5f) + 28;
                const Rect pill{right - w, r.cy() - 16, w, 32};
                if (row.focused)
                    list.bordered_rect(pill, 16, Color::rgb(0x000000, 0.0f), 1.5f, p.accent);
                else
                    list.rounded_rect(pill, 16, kWhite.with_alpha(0.08f));
                ui::text(list, fonts.semibold, row.badge, pill.cx(), pill.y + 22, 16,
                         row.focused ? p.accent : p.text_muted, Align::center, 1.5f);
                right = pill.x - 20;
            }
            const float text_w = right - (r.x + 88);
            ui::text(list, row.focused ? fonts.semibold : fonts.regular,
                     fit(fonts.semibold, row.title, 24, text_w), r.x + 88, r.y + 35, 24, kWhite);
            ui::text(list, fonts.regular, fit(fonts.regular, row.detail, 20, text_w), r.x + 88, r.y + 62,
                     20, row.focused ? kWhite.with_alpha(0.78f) : p.text_muted);
            if (row.progress >= 0)
            {
                const float f = tween::clamp01(static_cast<float>(row.progress) / 1000.0f);
                const Rect bar{r.x + 88, r.y + r.h - 6, text_w, 3};
                list.rounded_rect(bar, 1.5f, kWhite.with_alpha(0.12f));
                list.rounded_rect({bar.x, bar.y, std::max(3.0f, bar.w * f), bar.h}, 1.5f, p.accent);
            }
            list.pop_opacity();
        }
        if (has_focus_.value > 0.01f && menu_open_.value < 0.99f)
        {
            list.push_opacity(has_focus_.value * (1.0f - menu_open_.value));
            list.bordered_rect(ring_.value().inset(-3), 23, Color::rgb(0x000000, 0.0f), 3, kWhite);
            list.pop_opacity();
        }
    }

    /* Hints. */
    if (!hints_.empty())
    {
        ui::Hint hints[4];
        int n = 0;
        for (const auto &h : hints_)
            hints[n++] = {static_cast<ui::Button>(h.first), h.second.c_str()};
        /* As on the Twitch pages and Settings. */
        ui::HintLayout layout;
        layout.size = 34;
        layout.text_size = 24;
        layout.cy = 1036;
        list.push_opacity(tween::stagger(age_, 6, 0.06f, 0.4f));
        ui::draw_hints(list, fonts, ui::GlyphStyle::dark(), hints, n, kListRight, true, layout);
        list.pop_opacity();
    }

    /* OPTIONS menu. */
    const float m = menu_open_.value;
    if (m > 0.01f && !menu_.empty())
    {
        list.rounded_rect({0, 0, 1920, 1080}, 0, Color::rgb(0x000000, 0.45f * m));
        list.push_transform(0.96f + 0.04f * m, 960, 540, 0, 0);
        list.push_opacity(m);
        const Rect first = menu_item_rect(0);
        const float top = first.y - 180.0f;
        const float h = 180.0f + static_cast<float>(menu_.size()) * (kMenuItemH + 6.0f) + 24.0f;
        const Rect card{960.0f - kMenuW * 0.5f, top, kMenuW, h};
        list.shadow({card.x, card.y + 24, card.w, card.h}, 32, 48, Color::rgb(0x000000, 0.6f));
        list.rounded_rect(card, 32, p.bg_top.with_alpha(0.96f));
        list.bordered_rect(card, 32, Color::rgb(0x000000, 0.0f), 1.5f, kWhite.with_alpha(0.14f));
        const Rect well{card.x + 32, card.y + 36, 64, 64};
        list.rounded_rect(well, 18, p.accent);
        const std::uint32_t icon = ctx.textures.icon(menu_icon_);
        if (icon)
            list.image(icon, well.inset(16), ::hui::gfx::kFullUv, kInk);
        const float tx = well.x + well.w + 20;
        ui::text(list, fonts.semibold, ui::upper(menu_eyebrow_), tx, card.y + 54, 16, p.accent, Align::left,
                 3.0f);
        ui::text(list, fonts.display, fit(fonts.display, menu_title_, 32, card.x + card.w - 32 - tx), tx,
                 card.y + 92, 32, kWhite);
        ui::text(list, fonts.regular, fit(fonts.regular, menu_sub_, 19, card.w - 64), card.x + 32,
                 card.y + 144, 19, p.text_muted);

        list.rounded_rect(menu_ring_.value(), 20, kWhite);
        for (std::size_t i = 0; i < menu_.size(); ++i)
        {
            const MenuItem &item = menu_[i];
            const Rect r = menu_item_rect(static_cast<int>(i));
            const bool focused = static_cast<int>(i) == menu_focus_;
            const Color ink = focused ? kInk : (item.danger ? kDanger : kWhite);
            const std::uint32_t ic = ctx.textures.icon(item.icon);
            if (ic)
                list.image(ic, {r.x + 18, r.cy() - 14, 28, 28}, ::hui::gfx::kFullUv, ink);
            ui::text(list, fonts.semibold, item.label, r.x + 62, r.y + 30, 22, ink);
            ui::text(list, fonts.regular, fit(fonts.regular, item.desc, 16, r.w - 80), r.x + 62, r.y + 52, 16,
                     focused ? kInk.with_alpha(0.7f) : p.text_faint);
        }
        list.pop_opacity();
        list.pop_transform();
    }
}

bool ListScreen::focus(FocusInfo *out) const
{
    out->focusable.clear();
    if (!menu_.empty())
    {
        for (std::size_t i = 0; i < menu_.size(); ++i)
            out->focusable.emplace_back("menu-" + std::to_string(i), menu_[i].label);
        const int at = std::clamp(menu_focus_, 0, static_cast<int>(menu_.size()) - 1);
        out->id = "menu-" + std::to_string(at);
        out->text = menu_[static_cast<std::size_t>(at)].label;
        out->index = at + 1;
        out->total = static_cast<int>(menu_.size());
        out->rect = menu_item_rect(at);
        return true;
    }
    for (std::size_t i = 0; i < rows_.size(); ++i)
        out->focusable.emplace_back("row-" + std::to_string(i), rows_[i].title);
    for (std::size_t i = 0; i < rows_.size(); ++i)
    {
        if (!rows_[i].focused)
            continue;
        out->id = "row-" + std::to_string(i);
        out->text = rows_[i].title;
        out->index = cursor_ >= 0 ? cursor_ + 1 : static_cast<int>(i) + 1;
        out->total = total_ > 0 ? total_ : static_cast<int>(rows_.size());
        out->rect = row_rect(static_cast<int>(i));
        return true;
    }
    out->focusable.clear();
    return false;
}

/* ------------------------------------------------------------ focus report */

bool NavRail::focus(FocusInfo *out) const
{
    if (!params_.visible || !params_.rail_focused)
        return false;
    RailItem items[8];
    const int count = rail_items(items);
    const int at = std::clamp(params_.cursor_index, 0, count - 1);
    out->id = "nav-" + std::to_string(at);
    out->text = items[at].label;
    out->index = at + 1;
    out->total = count;
    out->rect = {28.0f, rail_item_y(at, count), kRailExpanded - 56.0f, 72.0f};
    out->focusable.clear();
    for (int i = 0; i < count; ++i)
        out->focusable.emplace_back("nav-" + std::to_string(i), items[i].label);
    return true;
}

} // namespace evo::kit
