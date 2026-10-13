/*
 * evo_hui_extra.cpp - EVO's on-screen keyboard on ps5-homebrew-ui.
 */
#include "evo_hui_extra.hpp"
#include "ui/glyphs.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

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
const Color kBlack = Color::rgb(0x000000);
const Color kClear = Color::rgb(0x000000, 0.0f);

/* The end of text that fits in width, after an ellipsis when the start had to
 * go: a text field shows where the typing happens. Measures only what fits,
 * so a 500-character chat message costs no more than a short one. */
std::string fit_tail(const ui::FontRef &font, const std::string &text, float size, float width)
{
    const hui::gfx::Font &f = *font.font;
    if (f.measure(text, size) <= width)
        return text;
    const std::string mark = f.has_glyph(0x2026) ? "\xE2\x80\xA6" : "...";
    std::string best = mark;
    std::size_t start = text.size();
    while (start > 0)
    {
        std::size_t prev = start - 1;
        while (prev > 0 && (static_cast<unsigned char>(text[prev]) & 0xC0) == 0x80)
            --prev;
        std::string candidate = mark + text.substr(prev);
        if (f.measure(candidate, size) > width)
            break;
        best = std::move(candidate);
        start = prev;
    }
    return best;
}

std::vector<std::string> split_utf8(const std::string& str) {
    std::vector<std::string> res;
    size_t i = 0;
    while (i < str.length()) {
        unsigned char c = str[i];
        size_t len = 1;
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        res.push_back(str.substr(i, len));
        i += len;
    }
    return res;
}
} // namespace

/* -------------------------------------------------------- KeyboardOverlay */

void KeyboardOverlay::set(const evo_keyboard_params_t &p)
{
    auto str = [](const char *s) { return std::string(s ? s : ""); };
    visible_ = p.visible != 0;
    native_only_ = p.native_only != 0;
    title_ = str(p.title);
    text_ = str(p.text);
    mode_label_ = str(p.mode_label);
    for (int i = 0; i < 4; ++i)
        rows_[i] = split_utf8(str(p.rows[i]));
    action_labels_.clear();
    for (int i = 0; i < 6; ++i) {
        if (p.action_labels[i] && p.action_labels[i][0] != '\0')
            action_labels_.push_back(p.action_labels[i]);
    }
    len_ = p.len;
    max_len_ = p.max_len;
    focus_row_ = p.focus_row;
    focus_col_ = p.focus_col;
    show_caret_ = p.show_caret != 0;
}

void KeyboardOverlay::enter()
{
    enter_scale_.snap(0.96f);
    enter_scale_.target = 1.0f;
    enter_fade_.snap(0.0f);
    enter_fade_.target = 1.0f;
    ring_snapped_ = false;
}

void KeyboardOverlay::update(float dt)
{
    enter_scale_.update(dt, 16.0f);
    enter_fade_.update(dt, 20.0f);

    Rect target{};
    if (focus_row_ >= 0 && focus_row_ < 4) {
        target = {340.0f + 140.0f + focus_col_ * 96.0f, 440.0f + 180.0f + focus_row_ * 72.0f, 96.0f, 72.0f};
    } else if (focus_row_ == 4 && focus_col_ >= 0 && focus_col_ < (int)action_labels_.size()) {
        float aw = 960.0f / std::max(1, (int)action_labels_.size());
        target = {340.0f + 140.0f + focus_col_ * aw, 440.0f + 180.0f + 4 * 72.0f + 20.0f, aw, 56.0f};
    }
    if (target.w > 0) {
        if (!ring_snapped_) {
            ring_.snap(target);
            ring_snapped_ = true;
        }
        ring_.target(target);
        ring_.update(dt, 24.0f);
    }
}

void KeyboardOverlay::draw(DrawList &list, const Context &ctx) const
{
    if (!visible_) return;
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;

    const float alpha = enter_fade_.value;
    list.rounded_rect({0, 0, 1920, 1080}, 0, kBlack.with_alpha(0.55f * alpha));

    if (native_only_) return;

    list.push_transform(enter_scale_.value, 960, 1080, 0, 0); // scale from bottom
    list.push_opacity(alpha);

    Rect card{340.0f, 440.0f, 1240.0f, 600.0f};
    list.shadow({card.x, card.y + 20, card.w, card.h}, 36, 60, kBlack.with_alpha(0.6f));
    list.rounded_rect(card, 36, p.bg_top.with_alpha(0.97f));
    list.bordered_rect(card, 36, kClear, 1.0f, kWhite.with_alpha(0.12f));

    // title
    ui::text(list, fonts.semibold, ui::upper(title_), card.cx(), card.y + 50, 16, p.text_muted, Align::center, 3.0f);

    // mode_label
    float mw = fonts.semibold.measure(mode_label_, 14, 1.5f) + 24;
    Rect mpill{card.x + card.w - 40 - mw, card.y + 34, mw, 28};
    list.rounded_rect(mpill, 14, kWhite.with_alpha(0.08f));
    ui::text(list, fonts.semibold, mode_label_, mpill.cx(), mpill.y + 19, 14, p.text_muted, Align::center, 1.5f);

    // text field
    Rect well{card.x + 80, card.y + 80, card.w - 160, 70};
    list.rounded_rect(well, 16, kBlack.with_alpha(0.3f));
    list.bordered_rect(well, 16, kClear, 1.0f, kWhite.with_alpha(0.06f));
    /* Room for the counter on the right ("500 / 500"). */
    const std::string shown = fit_tail(fonts.regular, text_, 28, well.w - 40 - 120);
    ui::text(list, fonts.regular, shown, well.x + 20, well.cy() + 10, 28, kWhite);
    if (show_caret_ && fmod(ctx.time, 1.0f) < 0.5f) {
        float tw = fonts.regular.measure(shown, 28);
        list.rounded_rect({well.x + 20 + tw + 2, well.cy() - 16, 2, 32}, 1, p.accent);
    }
    
    char counter[32];
    std::snprintf(counter, sizeof(counter), "%d / %d", len_, max_len_);
    ui::text(list, fonts.regular, counter, well.x + well.w - 20, well.cy() + 10, 16, p.text_faint, Align::right);

    // keys
    float kx = card.x + 140.0f;
    float ky = card.y + 180.0f;

    if (ring_snapped_) {
        list.rounded_rect(ring_.value(), 16, kWhite);
    }

    for (int r = 0; r < 4; ++r) {
        for (size_t c = 0; c < rows_[r].size() && c < 10; ++c) {
            bool focused = (focus_row_ == r && focus_col_ == (int)c);
            Color txt = focused ? kInk : kWhite;
            ui::text(list, fonts.regular, rows_[r][c], kx + c * 96.0f + 48.0f, ky + r * 72.0f + 48.0f, 32, txt, Align::center);
        }
    }

    // action bar
    if (!action_labels_.empty()) {
        float ay = ky + 4 * 72.0f + 20.0f;
        float aw = 960.0f / action_labels_.size();
        for (size_t i = 0; i < action_labels_.size(); ++i) {
            bool focused = (focus_row_ == 4 && focus_col_ == (int)i);
            Color txt = focused ? kInk : kWhite;
            if (!focused) {
                list.rounded_rect({kx + i * aw + 6, ay, aw - 12, 56.0f}, 28, kWhite.with_alpha(0.1f));
            }
            ui::text(list, fonts.semibold, action_labels_[i], kx + i * aw + aw * 0.5f, ay + 34.0f, 18, txt, Align::center);
        }
    }

    list.pop_opacity();
    list.pop_transform();
}

bool KeyboardOverlay::focus(FocusInfo *out) const
{
    if (!visible_ || native_only_) return false;
    out->focusable.clear();
    if (focus_row_ >= 0 && focus_row_ < 4) {
        out->id = "key-r" + std::to_string(focus_row_) + "c" + std::to_string(focus_col_);
        if (focus_col_ >= 0 && focus_col_ < (int)rows_[focus_row_].size())
            out->text = rows_[focus_row_][focus_col_];
        out->rect = {340.0f + 140.0f + focus_col_ * 96.0f, 440.0f + 180.0f + focus_row_ * 72.0f, 96.0f, 72.0f};
        return true;
    } else if (focus_row_ == 4 && focus_col_ >= 0 && focus_col_ < (int)action_labels_.size()) {
        out->id = "action-" + std::to_string(focus_col_);
        out->text = action_labels_[focus_col_];
        float aw = 960.0f / action_labels_.size();
        out->rect = {340.0f + 140.0f + focus_col_ * aw, 440.0f + 180.0f + 4 * 72.0f + 20.0f, aw, 56.0f};
        return true;
    }
    return false;
}

} // namespace evo::kit
