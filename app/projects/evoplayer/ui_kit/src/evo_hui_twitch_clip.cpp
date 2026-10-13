/*
 * evo_hui_twitch_clip.cpp - the card for a clip made in the player, on
 * ps5-homebrew-ui.
 *
 * A dark card at the bottom left of the picture: on its left the clip's page
 * as a QR on a white tile, on its right "CLIP MADE", "Scan to see your clip"
 * and "It's ready in a few seconds", with Circle's "Close" at the foot next to
 * a bar that runs down until the card closes. Its corner sits where the OSD's
 * margins are, scaled with the OSD's area (the video's box beside chat), so it
 * stays above the OSD's keys; its own size never scales, so the QR keeps
 * whole-pixel modules (as the login screen's does) and stays big enough to
 * scan from the couch.
 *
 * core/src/screens/PlayerScreen.cpp fills evo_hui_twitch_clip_params_t; this
 * only draws it.
 */
#include "evo_hui_twitch.hpp"

#include "ui/glyphs.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace evo::kit
{

using ::hui::gfx::Align;
using ::hui::gfx::Color;
using ::hui::gfx::DrawList;
using ::hui::gfx::Rect;
namespace ui = ::hui::ui;

namespace
{

const Color kWhite = Color::rgb(0xffffff);
const Color kInk = Color::rgb(0x0e0e10); /* the QR's modules: Twitch's near-black */
const Color kCard = Color::rgb(0x0e0e10, 0.94f);

/* Where the card's corner goes in the OSD's canvas: its left margin, and a
 * bottom clear of the OSD's keys (their row is centred on 1018). */
constexpr float kCornerX = 96.0f;
constexpr float kCornerBottom = 984.0f;

constexpr float kPad = 24.0f;    /* inside the card */
constexpr float kGap = 28.0f;    /* the tile to the text */
constexpr float kTextW = 330.0f; /* the text column */
constexpr float kRadius = 20.0f;
constexpr float kRise = 32.0f;   /* how far it rises as it comes in */

/* The QR: about this wide without its quiet zone, the tile never wider than
 * kQrMaxTile. A quiet zone of three modules: the tile's white is all there is
 * between the modules and the dark card. */
constexpr float kQrTarget = 200.0f;
constexpr int kQrQuiet = 3;
constexpr int kQrMaxTile = 240;
constexpr int kQrMinModule = 3;

/* The text column, from the card's top: the label, the line and the note. */
constexpr float kLabelY = 44.0f;
constexpr float kLabelSize = 16.0f;
constexpr float kTitleY = 94.0f;
constexpr float kTitleSize = 30.0f;
constexpr float kNoteY = 132.0f;
constexpr float kNoteSize = 22.0f;
/* Circle's hint at the foot, and the bar beside it. */
constexpr float kHintSize = 28.0f;
constexpr float kHintText = 20.0f;
constexpr float kBarH = 4.0f;

} // namespace

/* -------------------------------------------------------------------- state */

void TwitchClipCard::set(const evo_hui_twitch_clip_params_t &p)
{
    visible_ = p.visible != 0;
    time_left_ = std::isfinite(p.time_left) ? std::clamp(p.time_left, 0.0f, 1.0f) : 0.0f;
    area_ = Rect{0.0f, 0.0f, 1920.0f, 1080.0f};
    if (std::isfinite(p.area_x) && std::isfinite(p.area_y) && std::isfinite(p.area_w) && std::isfinite(p.area_h) &&
        p.area_w > 0.0f && p.area_h > 0.0f)
        area_ = Rect{p.area_x, p.area_y, p.area_w, p.area_h};

    /* Only a new QR is copied and cut into runs; a card fading out keeps its own. */
    if (!p.qr || p.qr_size <= 0)
        return;
    const std::size_t n = static_cast<std::size_t>(p.qr_size) * static_cast<std::size_t>(p.qr_size);
    if (p.qr_size == qr_size_ && qr_.size() == n &&
        std::equal(qr_.begin(), qr_.end(), p.qr, [](unsigned char a, unsigned char b) { return !a == !b; }))
        return;
    qr_size_ = p.qr_size;
    qr_.assign(p.qr, p.qr + n);
    runs_.clear();
    for (int y = 0; y < qr_size_; ++y)
    {
        const unsigned char *row = p.qr + static_cast<std::size_t>(y) * static_cast<std::size_t>(qr_size_);
        for (int x = 0; x < qr_size_;)
        {
            if (!row[x])
            {
                ++x;
                continue;
            }
            const int start = x;
            while (x < qr_size_ && row[x])
                ++x;
            runs_.push_back({y, start, x - start});
        }
    }
}

void TwitchClipCard::enter()
{
    /* It has just appeared: it rises in from nothing. */
    in_.snap(0.0f);
}

void TwitchClipCard::update(float dt)
{
    in_.target = visible_ && qr_size_ > 0 ? 1.0f : 0.0f;
    in_.update(dt, 14.0f);
}

bool TwitchClipCard::focus(FocusInfo *out) const
{
    out->focusable.clear();
    return false;
}

int TwitchClipCard::module_px() const
{
    if (qr_size_ <= 0)
        return 0;
    int m = std::max(kQrMinModule, static_cast<int>(std::lround(kQrTarget / static_cast<float>(qr_size_))));
    while (m > kQrMinModule && (qr_size_ + 2 * kQrQuiet) * m > kQrMaxTile)
        --m;
    return m;
}

Rect TwitchClipCard::card_rect() const
{
    const float tile = static_cast<float>((qr_size_ + 2 * kQrQuiet) * module_px());
    const float w = kPad + tile + kGap + kTextW + kPad;
    const float h = std::max(tile + 2.0f * kPad, 200.0f);
    const float s = area_.w / 1920.0f;
    /* Whole canvas pixels, so the modules land on whole screen pixels. */
    const float x = std::round(area_.x + kCornerX * s);
    const float bottom = std::round(area_.y + kCornerBottom * s);
    return {x, bottom - h, w, h};
}

/* --------------------------------------------------------------------- draw */

void TwitchClipCard::draw(DrawList &list, const Context &ctx) const
{
    const float in = in_.value;
    if (in <= 0.01f || qr_size_ <= 0)
        return;
    const ui::Fonts &fonts = ctx.fonts;
    const Rect card = card_rect();
    const int m = module_px();
    const float mf = static_cast<float>(m);
    const float side = static_cast<float>((qr_size_ + 2 * kQrQuiet) * m);

    /* Whole pixels while it moves too, or the modules would blur and seam. */
    list.push_transform(1.0f, 0.0f, 0.0f, 0.0f, std::round((1.0f - in) * kRise));
    list.push_opacity(in);

    list.shadow({card.x, card.y + 16, card.w, card.h}, kRadius, 40, Color::rgb(0x000000, 0.55f));
    list.bordered_rect(card, kRadius, kCard, 1.0f, kWhite.with_alpha(0.12f));

    /* The QR on its white tile, centred down the card's left. */
    const Rect tile{card.x + kPad, card.y + std::round((card.h - side) * 0.5f), side, side};
    list.rounded_rect(tile, std::min(12.0f, 2.0f * mf), kWhite);
    const float ox = tile.x + static_cast<float>(kQrQuiet) * mf;
    const float oy = tile.y + static_cast<float>(kQrQuiet) * mf;
    for (const QrRun &r : runs_)
        list.rounded_rect({ox + static_cast<float>(r.col) * mf, oy + static_cast<float>(r.row) * mf,
                           static_cast<float>(r.len) * mf, mf},
                          0, kInk);

    /* The words. */
    const float tx = tile.x + tile.w + kGap;
    ui::text(list, fonts.semibold, "CLIP MADE", tx, card.y + kLabelY, kLabelSize, ctx.palette.accent, Align::left,
             2.5f);
    ui::text(list, fonts.semibold, "Scan to see your clip", tx, card.y + kTitleY, kTitleSize, kWhite);
    ui::text(list, fonts.regular, "It's ready in a few seconds", tx, card.y + kNoteY, kNoteSize,
             ctx.palette.text_muted);

    /* Circle closes it, and the bar beside it shows how long it stays. */
    const float cy = card.y + card.h - kPad - kHintSize * 0.5f;
    const ui::Hint close{ui::Button::circle, "Close"};
    ui::HintLayout layout;
    layout.size = kHintSize;
    layout.text_size = kHintText;
    layout.cy = cy;
    layout.item_gap = 0.0f;
    const float hint_w = ui::draw_hints(list, fonts, ui::GlyphStyle::dark(), &close, 1, tx, false, layout);
    const float bar_x = tx + hint_w + 24.0f;
    const float bar_w = card.x + card.w - kPad - bar_x;
    if (bar_w > 8.0f)
    {
        const Rect track{bar_x, std::round(cy - kBarH * 0.5f), bar_w, kBarH};
        list.rounded_rect(track, kBarH * 0.5f, kWhite.with_alpha(0.14f));
        if (time_left_ > 0.0f)
            list.rounded_rect({track.x, track.y, std::max(kBarH, track.w * time_left_), track.h}, kBarH * 0.5f,
                              ctx.palette.accent);
    }

    list.pop_opacity();
    list.pop_transform();
}

} // namespace evo::kit
