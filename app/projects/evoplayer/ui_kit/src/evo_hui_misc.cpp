#include "evo_hui_misc.hpp"

#include "evo_about_text.hpp"
#include "evo_hui_twitch.hpp"

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

std::string fit(const ui::FontRef &font, const std::string &text, float size, float width)
{
    return font.font->fit(text, size, width);
}

} // namespace

/* ----------------------------------------------------------- DialogOverlay */

void DialogOverlay::set(const evo_rmlui_dialog_params_t &p)
{
    auto str = [](const char *s) { return std::string(s ? s : ""); };
    eyebrow_ = str(p.eyebrow);
    title_ = str(p.title);
    detail_ = str(p.detail);
    progress_pct_ = p.progress_pct;
    focused_action_ = p.focused_action;
    actions_.clear();
    for (int i = 0; i < p.action_count && i < 3; ++i)
    {
        Action a;
        a.icon_path = str(p.actions[i].icon_path);
        a.label = str(p.actions[i].label);
        a.is_primary = p.actions[i].is_primary != 0;
        actions_.push_back(a);
    }
}

void DialogOverlay::enter()
{
    enter_scale_.snap(0.95f);
    enter_scale_.target = 1.0f;
    enter_fade_.snap(0.0f);
    enter_fade_.target = 1.0f;
}

void DialogOverlay::update(float dt)
{
    enter_scale_.update(dt, 16.0f);
    enter_fade_.update(dt, 20.0f);
}

void DialogOverlay::draw(DrawList &list, const Context &ctx) const
{
    const float alpha = enter_fade_.value;
    if (alpha < 0.01f)
        return;

    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;

    list.rounded_rect({0, 0, 1920, 1080}, 0, Color::rgb(0x000000, 0.5f * alpha));

    const float scale = enter_scale_.value;
    list.push_transform(scale, 960, 540, 0, 0);
    list.push_opacity(alpha);

    const float w = 760.0f;
    float h = 64.0f; 
    if (!eyebrow_.empty()) h += 24.0f;
    h += 50.0f;
    if (!detail_.empty()) h += 32.0f * std::min(4, (int)fonts.regular.font->wrap(detail_, 24, w - 80).size()) + 20.0f;
    if (progress_pct_ >= 0.0) h += 30.0f;
    if (!actions_.empty()) h += 80.0f;

    const Rect card{960.0f - w * 0.5f, 540.0f - h * 0.5f, w, h};
    list.shadow({card.x, card.y + 24, card.w, card.h}, 48, 64, Color::rgb(0x000000, 0.6f));
    list.rounded_rect(card, 32, p.bg_top.with_alpha(0.97f));
    list.bordered_rect(card, 32, Color::rgb(0,0), 1.0f, kWhite.with_alpha(0.14f));

    float cy = card.y + 40.0f;
    if (!eyebrow_.empty())
    {
        ui::text(list, fonts.semibold, ui::upper(eyebrow_), card.cx(), cy, 16, p.accent, Align::center, 3.0f);
        cy += 30.0f;
    }
    ui::text(list, fonts.display, fit(fonts.display, title_, 38, w - 80), card.cx(), cy + 24, 38, kWhite, Align::center);
    cy += 50.0f;

    if (!detail_.empty())
    {
        cy = ui::paragraph(list, fonts.regular, detail_, card.cx(), cy + 20, 24, w - 80, 32.0f, p.text_muted, 4, Align::center);
        cy += 10.0f;
    }

    if (progress_pct_ >= 0)
    {
        const Rect bar{card.x + 40, cy + 10, w - 80, 6};
        list.rounded_rect(bar, 3, kWhite.with_alpha(0.16f));
        list.rounded_rect({bar.x, bar.y, std::max(6.0f, bar.w * (float)progress_pct_), bar.h}, 3, p.accent);
        cy += 30.0f;
    }

    if (!actions_.empty())
    {
        float aw = (w - 80 - (actions_.size() - 1) * 20.0f) / actions_.size();
        for (std::size_t i = 0; i < actions_.size(); ++i)
        {
            Rect btn{card.x + 40 + i * (aw + 20), card.y + card.h - 80, aw, 48};
            bool focused = (int)i == focused_action_;
            if (focused)
            {
                list.rounded_rect(btn, 24, kWhite);
            }
            else
            {
                list.rounded_rect(btn, 24, kWhite.with_alpha(0.1f));
                if (actions_[i].is_primary)
                    list.bordered_rect(btn, 24, Color::rgb(0,0), 2.0f, p.accent);
            }
            Color txt = focused ? kInk : kWhite;
            ui::text(list, fonts.semibold, fit(fonts.semibold, actions_[i].label, 18, aw - 20), btn.cx(), btn.cy() + 7, 18, txt, Align::center);
        }
    }

    list.pop_opacity();
    list.pop_transform();
}

bool DialogOverlay::focus(FocusInfo *out) const
{
    if (focused_action_ >= 0 && focused_action_ < (int)actions_.size())
    {
        out->focusable.clear();
        for (size_t i = 0; i < actions_.size(); ++i)
            out->focusable.emplace_back("action-" + std::to_string(i), actions_[i].label);
        out->id = "action-" + std::to_string(focused_action_);
        out->text = actions_[focused_action_].label;
        out->index = focused_action_ + 1;
        out->total = static_cast<int>(actions_.size());
        out->rect = {620.0f, 580.0f, 680.0f, 48.0f};
        return true;
    }
    return false;
}

/* ------------------------------------------------------------ ToastOverlay */

void ToastOverlay::set(const evo_rmlui_toast_params_t &p)
{
    auto str = [](const char *s) { return std::string(s ? s : ""); };
    title_ = str(p.title);
    message_ = str(p.message);
    kind_ = p.kind;
    if (!visible_ && p.visible)
        bottom_snapped_ = false;
    visible_ = p.visible != 0;
    alpha_ = p.alpha;
    slide_ = p.slide;
    bottom_target_ = p.bottom > 0 ? static_cast<float>(p.bottom) : 990.0f;
}

void ToastOverlay::enter()
{
}

void ToastOverlay::update(float dt)
{
    /* Up out of the way when the player's timeline comes up under it, and
     * back down when it goes; a new toast starts where it belongs. */
    if (!bottom_snapped_)
    {
        bottom_ = bottom_target_;
        bottom_snapped_ = true;
        return;
    }
    bottom_ += (bottom_target_ - bottom_) * std::min(1.0f, dt * 12.0f);
}

void ToastOverlay::draw(DrawList &list, const Context &ctx) const
{
    if (!visible_ || alpha_ <= 0)
        return;
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;

    /* The bar down the left edge says what kind it is (evo_toast.h): red
     * when something didn't work, green when something finished, the accent
     * for the rest. */
    Color acc;
    if (kind_ == 1) acc = Color::rgb(0x8a8fff);
    else if (kind_ == 2) acc = Color::rgb(0xff5a5f);
    else if (kind_ == 3) acc = Color::rgb(0x30d158);
    else acc = p.accent;

    list.push_opacity((float)alpha_ / 255.0f);

    /* Sized to be read from the couch. A toast with only a title or only a
     * message is one line, in the title's type ("Starting NordlichtTV...");
     * with both, the message goes under the title. */
    constexpr float kHeadSize = 24.0f, kBodySize = 22.0f, kMaxW = 820.0f;
    constexpr float kTextX = 42.0f, kPadR = 28.0f;
    const std::string head = drawable(fonts.semibold, title_.empty() ? message_ : title_);
    const std::string body = title_.empty() ? std::string() : drawable(fonts.regular, message_);
    const bool two = !body.empty();
    const float text_w = std::max(fonts.semibold.measure(head, kHeadSize),
                                  two ? fonts.regular.measure(body, kBodySize) : 0.0f);
    const float w = std::min(kMaxW, text_w + kTextX + kPadR);
    const float h = two ? 88.0f : 64.0f;
    /* Right-aligned with the pages and their hint bars, just above them (or
     * above the player's timeline). */
    Rect card{tw::kRight - w + slide_, bottom_ - h, w, h};

    list.shadow({card.x, card.y + 10, card.w, card.h}, 20, 24, Color::rgb(0x000000, 0.5f));
    list.rounded_rect(card, 20, p.bg_top.with_alpha(0.96f));
    list.bordered_rect(card, 20, Color::rgb(0,0), 1.0f, kWhite.with_alpha(0.12f));

    list.rounded_rect({card.x + 18, card.y + 16, 6, h - 32}, 3, acc);

    const float fit_w = w - kTextX - kPadR;
    if (two)
    {
        ui::text(list, fonts.semibold, fit(fonts.semibold, head, kHeadSize, fit_w), card.x + kTextX, card.y + 37,
                 kHeadSize, kWhite);
        ui::text(list, fonts.regular, fit(fonts.regular, body, kBodySize, fit_w), card.x + kTextX, card.y + 66,
                 kBodySize, p.text_muted);
    }
    else
    {
        ui::text(list, fonts.semibold, fit(fonts.semibold, head, kHeadSize, fit_w), card.x + kTextX, card.y + 41,
                 kHeadSize, kWhite);
    }

    list.pop_opacity();
}

bool ToastOverlay::focus(FocusInfo *out) const
{
    return false;
}

/* ------------------------------------------------------------- AboutScreen */

void AboutScreen::set(const evo_rmlui_about_params_t &p)
{
    auto str = [](const char *s) { return std::string(s ? s : ""); };
    app_name_ = str(p.app_name);
    version_ = str(p.version);
    title_id_ = str(p.build_tag);
    rail_focused_ = p.action_focused == 0;
}

void AboutScreen::enter()
{
    age_ = 0.0f;
}

void AboutScreen::update(float dt)
{
    age_ += dt;
}

/*
 * Laid out like a channel page: the mark where a channel's avatar sits, the
 * name and a line under it on the title and line baselines the Twitch screens
 * share, then the license notice and the source line on the left, and from
 * Home's picture edge what the app is built from: a grid of two columns, each
 * entry its name over "author · license". All of it fits above the hint band.
 */
void AboutScreen::draw(DrawList &list, const Context &ctx) const
{
    namespace about = ::evo::about;
    draw_backdrop(list, ctx);
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;

    constexpr float kMarkW = 128.0f;
    constexpr float kNameX = tw::kLeft + kMarkW + 40.0f;
    constexpr float kColTop = 328.0f;  /* the columns' headings */
    constexpr float kLeftW = 720.0f;
    constexpr float kRightX = 1024.0f; /* Home's picture edge */
    constexpr float kText = 24.0f, kLead = 36.0f, kParaGap = 16.0f;

    Lines &l = lines_;
    if (l.fonts != &ctx.fonts)
    {
        l.fonts = &ctx.fonts;
        l.license.clear();
        for (int i = 0; i < about::kLicenseCount; ++i)
            l.license.push_back(fit_lines(fonts.regular, about::kLicense[i], kText, kLeftW, 6));
        l.source = fit_lines(fonts.regular, about::kSource, kText, kLeftW, 3);
    }

    const Color body = p.text.with_alpha(0.86f);
    {
        const float in = tween::stagger(age_, 0, 0.05f, 0.45f);
        list.push_opacity(in);
        list.push_transform(1.0f, 0, 0, 0, 16.0f * (1.0f - in));
        draw_app_mark(list, ctx, tw::kLeft, 76.0f, kMarkW);
        ui::text(list, fonts.display, app_name_, kNameX - 2, 136, 56, p.text);
        std::string line = "Version " + version_;
        if (!title_id_.empty())
            line += " \xC2\xB7 " + title_id_;
        ui::text(list, fonts.regular, line, kNameX, 184, 26, p.text_muted);
        ui::text(list, fonts.regular, std::string(about::kBasedOn) + " " + about::kNotAffiliated, kNameX, 232, kText,
                 p.text.with_alpha(0.78f));
        list.pop_transform();
        list.pop_opacity();
    }

    const float in = tween::stagger(age_, 1, 0.08f, 0.6f);
    list.push_opacity(in);
    list.push_transform(1.0f, 0, 0, 0, 30.0f * (1.0f - in));

    /* Left: the license notice, then where the source is. */
    ui::text(list, fonts.semibold, "License", tw::kLeft, kColTop, 30, p.text);
    float y = kColTop + 56.0f;
    for (const std::vector<std::string> &para : l.license)
    {
        for (const std::string &s : para)
        {
            ui::text(list, fonts.regular, s, tw::kLeft, y, kText, body);
            y += kLead;
        }
        y += kParaGap;
    }
    y += 40.0f;
    ui::text(list, fonts.semibold, "Source code", tw::kLeft, y, 30, p.text);
    y += 56.0f;
    for (const std::string &s : l.source)
    {
        ui::text(list, fonts.regular, s, tw::kLeft, y, kText, body);
        y += kLead;
    }

    /* Under the source line, EVO's project page. */
    y += kParaGap;
    ui::text(list, fonts.regular, about::kEvoProject, tw::kLeft, y, kText, p.text_muted);

    /* Right: what the app is built from, filled down the first column, then
     * the second. */
    ui::text(list, fonts.semibold, "Built with", kRightX, kColTop, 30, p.text);
    constexpr int kRows = (about::kCreditCount + 1) / 2;
    constexpr float kColW = (tw::kRight - kRightX) * 0.5f;
    constexpr float kCellPitch = 66.0f, kSubDrop = 29.0f;
    for (int i = 0; i < about::kCreditCount; ++i)
    {
        const about::Credit &c = about::kCredits[i];
        const float x = kRightX + static_cast<float>(i / kRows) * kColW;
        const float base = kColTop + 56.0f + static_cast<float>(i % kRows) * kCellPitch;
        ui::text(list, fonts.regular, c.what, x, base, kText, body);
        std::string sub = c.license;
        if (c.who[0])
            sub = std::string(c.who) + " \xC2\xB7 " + sub;
        ui::text(list, fonts.regular, sub, x, base + kSubDrop, 20, p.text_muted);
    }

    list.pop_transform();
    list.pop_opacity();

    ui::Hint hints[2];
    int h = 0;
    if (rail_focused_)
        h = rail_hints(hints);
    else
        hints[h++] = {ui::Button::circle, "Back"};
    draw_twitch_hints(list, ctx, hints, h, tween::stagger(age_, 6, 0.08f, 0.5f));
}

bool AboutScreen::focus(FocusInfo *out) const
{
    /* Nothing to pick: the page reads as one item for the dev remote. */
    out->focusable.clear();
    if (rail_focused_)
        return false;
    out->id = "about";
    out->text = app_name_;
    out->index = out->total = 1;
    out->rect = {tw::kLeft, 76.0f, tw::kView, 800.0f};
    return true;
}

/* ------------------------------------------------------------ ClosedScreen */

void ClosedScreen::enter() {}
void ClosedScreen::update(float dt) {}

void ClosedScreen::draw(DrawList &list, const Context &ctx) const
{
    draw_backdrop(list, ctx);
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;

    float cx = 1920.0f * 0.5f;
    draw_app_mark(list, ctx, cx - 64, 244, 128);

    ui::text(list, fonts.display, ::evo::about::kAppName, cx, 440, 56, p.text, Align::center);
    ui::text(list, fonts.semibold, "Safe to close", cx, 490, 28, p.accent_alt, Align::center);

    ui::text(list, fonts.regular, "Press the PS button to open the control centre.", cx, 550, 20, p.text_muted, Align::center);
    ui::text(list, fonts.regular, std::string("Highlight ") + ::evo::about::kAppName + " and close it from the options menu.",
             cx, 580, 20, p.text_muted, Align::center);
}

bool ClosedScreen::focus(FocusInfo *out) const
{
    return false;
}

} // namespace evo::kit
