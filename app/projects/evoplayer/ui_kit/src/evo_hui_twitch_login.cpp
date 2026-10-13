/*
 * evo_hui_twitch_login.cpp - logging in to Twitch, on ps5-homebrew-ui.
 *
 * Full screen, no rail. On the left, the title and two numbered steps: go to
 * twitch.tv/activate, then enter the code, which sits in large letters in a
 * box with "The code changes in N min" under it. On the right, the QR (it
 * opens the activate page with the code filled in) on a white tile with a
 * quiet zone of four modules, and "Or scan this with your phone" under it.
 * While a code is on its way the box spins and the tile is a skeleton. The
 * check of the saved login, a failure or the login itself fades the steps
 * out for one centred message.
 *
 * The QR is one rect per run of dark modules in a row, with whole-pixel
 * modules on a whole-pixel tile: at 1080p and at 4K (the kit's canvas at
 * scale 1 and 2) every edge falls between screen pixels, so neighbouring
 * modules meet without a seam.
 *
 * core/src/screens/TwitchLoginScreen.cpp follows the login service and fills
 * evo_hui_twitch_login_params_t; this only draws it.
 */
#include "evo_hui_twitch.hpp"

#include "ui/glyphs.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

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
const Color kClear = Color::rgb(0x000000, 0.0f);
const Color kInk = Color::rgb(0x0e0e10); /* the QR's modules: Twitch's near-black */

/* The page is centred on the whole canvas (there is no rail): the steps
 * from kColL, the QR tile ending at kColR. */
constexpr float kColL = 240.0f;
constexpr float kColR = 1680.0f;
constexpr float kMid = 960.0f;

/* The mark and the title. The page, mark to caption, sits in the middle of
 * what the hint bar leaves. */
constexpr float kMarkY = 186.0f;
constexpr float kMarkW = 60.0f;
constexpr float kTitleY = 336.0f;
constexpr float kTitleSize = 72.0f;

/* The steps: a numbered disc, its line beside it. */
constexpr float kDiscR = 22.0f;
constexpr float kStepX = kColL + 2.0f * kDiscR + 24.0f;
constexpr float kStepSize = 30.0f;
constexpr float kStep1Y = 428.0f;
constexpr float kUrlY = 488.0f;
constexpr float kUrlSize = 44.0f;
constexpr float kStep2Y = 576.0f;

/* The code in its box, and the line under it. */
const Rect kCodeBox{kStepX, 608.0f, 640.0f, 136.0f};
constexpr float kCodeSize = 84.0f;
constexpr float kCodeTracking = 6.0f;
constexpr float kCodeGap = 56.0f; /* between the two groups of four: wider than a letter */
constexpr float kExpiryY = 798.0f; /* also the QR caption's baseline */

/* The QR: about this wide without its quiet zone, the tile never wider than
 * kQrMaxTile, its bottom on the code box's and its caption on the expiry
 * line's baseline. The skeleton stands in at kQrSkeleton while there is none. */
constexpr float kQrTarget = 360.0f;
constexpr int kQrQuiet = 4; /* modules of white around it */
constexpr int kQrMaxTile = 480;
constexpr float kQrSkeleton = 450.0f;

/* A failure or the login, on its own in the middle: the steps and the
 * title make way for it. */
constexpr float kMessageMarkY = 330.0f;
constexpr float kHeadlineY = 534.0f;
constexpr float kHeadlineSize = 56.0f;
constexpr float kMessageY = 598.0f; /* the sentence's first baseline */
constexpr float kMessageSize = 30.0f;
constexpr float kMessageLead = 42.0f;
constexpr float kMessageW = 1000.0f;
constexpr float kNameW = 1200.0f;   /* "Logged in as <name>" */
constexpr float kTickY = 440.0f;    /* the done tick's centre */
constexpr float kTickR = 64.0f;
constexpr float kDoneY = 590.0f;
constexpr float kDoneSize = 52.0f;
constexpr float kCheckingY = 470.0f; /* the spinner's centre while the saved login is read */
constexpr float kCheckingTextY = 574.0f;

const char *const kLoggedInAs = "Logged in as ";

void draw_step_disc(DrawList &list, const Context &ctx, const char *number, float baseline)
{
    const float cx = kColL + kDiscR, cy = baseline - 11.0f; /* on the line's lower-case middle */
    list.circle(cx, cy, kDiscR, ctx.palette.accent);
    ui::text(list, ctx.fonts.semibold, number, cx, cy + 8.5f, 24, kWhite, Align::center);
}

} // namespace

/* -------------------------------------------------------------------- state */

void TwitchLoginScreen::set(const evo_hui_twitch_login_params_t &p)
{
    state_ = p.state;
    seconds_left_ = p.seconds_left;
    /* Each text stays when it goes empty, so the part that showed it can
     * fade out with it still there. Same text: no copy, no allocation. */
    auto take = [&](std::string &dst, const char *src, bool counts) {
        if (!src || !*src || dst == src)
            return;
        dst = src;
        if (counts)
            ++rev_;
    };
    take(code_, p.code, false);
    take(name_, p.name, true);
    take(login_, p.login, true);
    take(error_, p.error, true);
    if (p.qr && p.qr_size > 0)
        set_qr(p.qr, p.qr_size);
    else if (state_ == EVO_HUI_TWITCH_LOGIN_WAITING)
        clear_qr(); /* a new code with no QR: the last code's must not stay up */
}

void TwitchLoginScreen::clear_qr()
{
    qr_size_ = 0;
    qr_.clear();
    runs_.clear();
    qr_in_.snap(0.0f);
}

void TwitchLoginScreen::set_qr(const unsigned char *dark, int size)
{
    const std::size_t n = static_cast<std::size_t>(size) * static_cast<std::size_t>(size);
    if (size == qr_size_ && qr_.size() == n && std::equal(qr_.begin(), qr_.end(), dark,
                                                         [](unsigned char a, unsigned char b) { return !a == !b; }))
        return;
    /* Another code: its QR fades in afresh. */
    qr_size_ = size;
    qr_.assign(dark, dark + n);
    runs_.clear();
    for (int y = 0; y < size; ++y)
    {
        const unsigned char *row = dark + static_cast<std::size_t>(y) * static_cast<std::size_t>(size);
        for (int x = 0; x < size;)
        {
            if (!row[x])
            {
                ++x;
                continue;
            }
            const int start = x;
            while (x < size && row[x])
                ++x;
            runs_.push_back({y, start, x - start});
        }
    }
    qr_in_.snap(0.0f);
}

void TwitchLoginScreen::enter()
{
    age_ = 0.0f;
    done_age_ = 0.0f;
    /* set() ran first: what is already there shows at once. */
    const bool steps = state_ == EVO_HUI_TWITCH_LOGIN_STARTING || state_ == EVO_HUI_TWITCH_LOGIN_WAITING;
    steps_in_.snap(steps ? 1.0f : 0.0f);
    code_in_.snap(state_ == EVO_HUI_TWITCH_LOGIN_WAITING && !code_.empty() ? 1.0f : 0.0f);
    qr_in_.snap(state_ == EVO_HUI_TWITCH_LOGIN_WAITING && qr_size_ > 0 ? 1.0f : 0.0f);
    checking_in_.snap(state_ == EVO_HUI_TWITCH_LOGIN_CHECKING ? 1.0f : 0.0f);
    error_in_.snap(state_ == EVO_HUI_TWITCH_LOGIN_ERROR ? 1.0f : 0.0f);
    done_in_.snap(state_ == EVO_HUI_TWITCH_LOGIN_DONE ? 1.0f : 0.0f);
}

void TwitchLoginScreen::update(float dt)
{
    age_ += dt;
    const bool waiting = state_ == EVO_HUI_TWITCH_LOGIN_WAITING;
    steps_in_.target = state_ == EVO_HUI_TWITCH_LOGIN_STARTING || waiting ? 1.0f : 0.0f;
    code_in_.target = waiting && !code_.empty() ? 1.0f : 0.0f;
    qr_in_.target = waiting && qr_size_ > 0 ? 1.0f : 0.0f;
    checking_in_.target = state_ == EVO_HUI_TWITCH_LOGIN_CHECKING ? 1.0f : 0.0f;
    error_in_.target = state_ == EVO_HUI_TWITCH_LOGIN_ERROR ? 1.0f : 0.0f;
    done_in_.target = state_ == EVO_HUI_TWITCH_LOGIN_DONE ? 1.0f : 0.0f;
    checking_in_.update(dt, 10.0f);
    steps_in_.update(dt, 10.0f);
    code_in_.update(dt, 10.0f);
    qr_in_.update(dt, 8.0f);
    error_in_.update(dt, 10.0f);
    done_in_.update(dt, 10.0f);
    done_age_ = state_ == EVO_HUI_TWITCH_LOGIN_DONE ? done_age_ + dt : 0.0f;
}

int TwitchLoginScreen::module_px() const
{
    if (qr_size_ <= 0)
        return 0;
    int m = std::max(2, static_cast<int>(std::lround(kQrTarget / static_cast<float>(qr_size_))));
    while (m > 2 && (qr_size_ + 2 * kQrQuiet) * m > kQrMaxTile)
        --m;
    return m;
}

Rect TwitchLoginScreen::qr_tile() const
{
    const int m = module_px();
    const float side = m > 0 ? static_cast<float>((qr_size_ + 2 * kQrQuiet) * m) : kQrSkeleton;
    /* All whole numbers: the constants are, and so is the side. */
    return {kColR - side, kCodeBox.y + kCodeBox.h - side, side, side};
}

const std::string &TwitchLoginScreen::expiry_text() const
{
    const int minutes = seconds_left_ < 60 ? 0 : (seconds_left_ + 59) / 60;
    if (minutes != expiry_minutes_)
    {
        expiry_minutes_ = minutes;
        char b[64];
        if (minutes == 0)
            std::snprintf(b, sizeof b, "The code changes in less than a minute");
        else
            std::snprintf(b, sizeof b, "The code changes in %d min", minutes);
        expiry_ = b;
    }
    return expiry_;
}

/* ------------------------------------------------------------------- header */

void TwitchLoginScreen::draw_header(DrawList &list, const Context &ctx, float alpha) const
{
    list.push_opacity(alpha);
    draw_app_mark(list, ctx, kColL, kMarkY, kMarkW);
    ui::text(list, ctx.fonts.display, "Log in to Twitch", kColL - 4, kTitleY, kTitleSize, ctx.palette.text);
    list.pop_opacity();
}

/* -------------------------------------------------------------------- steps */

void TwitchLoginScreen::draw_steps(DrawList &list, const Context &ctx, float alpha) const
{
    if (alpha <= 0.01f)
        return;
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    list.push_opacity(alpha);

    draw_step_disc(list, ctx, "1", kStep1Y);
    ui::text(list, fonts.regular, "On your phone or computer, go to", kStepX, kStep1Y, kStepSize,
             p.text.with_alpha(0.86f));
    ui::text(list, fonts.semibold, "twitch.tv/activate", kStepX - 1, kUrlY, kUrlSize, p.accent_alt);

    draw_step_disc(list, ctx, "2", kStep2Y);
    ui::text(list, fonts.regular, "Enter this code", kStepX, kStep2Y, kStepSize, p.text.with_alpha(0.86f));

    /* The code's box: lit while it holds a code, quiet while one comes. */
    const Rect box = kCodeBox;
    const float lit = code_in_.value;
    if (lit > 0.01f)
        list.glow(box, 20, 30, p.accent.with_alpha(0.28f * lit));
    list.rounded_rect(box, 20, p.surface_sel);
    list.bordered_rect(box, 20, kClear, 2.0f, ::hui::gfx::mix(kWhite.with_alpha(0.08f), p.accent, lit));

    if (lit < 0.99f && (state_ == EVO_HUI_TWITCH_LOGIN_STARTING || state_ == EVO_HUI_TWITCH_LOGIN_WAITING))
    {
        /* A spinner and "Getting a code", centred as one (not while the
         * steps make way for a message). */
        list.push_opacity(1.0f - lit);
        const char *getting = "Getting a code";
        const float size = 28.0f, r = 16.0f, gap = 18.0f;
        const float w = 2.0f * r + gap + fonts.semibold.measure(getting, size);
        const float x = box.cx() - w * 0.5f;
        draw_spinner(list, ctx, x + r, box.cy(), r, p.text_muted);
        ui::text(list, fonts.semibold, getting, x + 2.0f * r + gap, box.cy() + 10, size, p.text_muted);
        list.pop_opacity();
    }
    if (lit > 0.01f && !code_.empty())
    {
        /* Twitch's codes are eight letters, drawn as two groups of four with
         * a gap between them: a visible space, not a hyphen glyph someone
         * reading the box by eye might also type at twitch.tv/activate.
         * Anything else (defensive, in case the shape ever differs) draws as
         * one block. Shrunk, never cut, if it runs longer than the box. */
        const bool paired = code_.size() == 8;
        const std::string left = paired ? code_.substr(0, 4) : code_;
        const std::string right = paired ? code_.substr(4) : std::string();
        const float leftW = fonts.display.measure(left, kCodeSize, kCodeTracking);
        const float rightW = paired ? fonts.display.measure(right, kCodeSize, kCodeTracking) : 0.0f;
        const float natural = paired ? leftW + kCodeGap + rightW : leftW;
        const float room = box.w - 64.0f;
        const float scale = natural > room ? room / natural : 1.0f;
        const float size = kCodeSize * scale;
        const float tracking = kCodeTracking * scale;
        /* The caps' middle on the box's: Montserrat's caps are 0.7 em. */
        const float baseline = box.cy() + std::round(size * 0.35f);
        list.push_opacity(lit);
        if (paired)
        {
            const float x0 = box.cx() - natural * scale * 0.5f;
            ui::text(list, fonts.display, left, x0, baseline, size, p.text, Align::left, tracking);
            ui::text(list, fonts.display, right, x0 + (leftW + kCodeGap) * scale, baseline, size, p.text,
                     Align::left, tracking);
        }
        else
        {
            ui::text(list, fonts.display, code_, box.cx(), baseline, size, p.text, Align::center, tracking);
        }
        list.pop_opacity();
    }

    if (seconds_left_ >= 0 && state_ == EVO_HUI_TWITCH_LOGIN_WAITING)
    {
        list.push_opacity(lit);
        ui::text(list, fonts.regular, expiry_text(), kStepX, kExpiryY, 24, p.text_muted);
        list.pop_opacity();
    }

    list.pop_opacity();
}

/* ----------------------------------------------------------------------- QR */

void TwitchLoginScreen::draw_qr(DrawList &list, const Context &ctx, float alpha) const
{
    if (alpha <= 0.01f)
        return;
    const Rect tile = qr_tile();
    const float in = qr_in_.value;
    /* The skeleton while the code comes, and under the QR as it fades in. */
    const bool coming = state_ == EVO_HUI_TWITCH_LOGIN_STARTING || qr_size_ > 0;
    if (coming && in < 0.99f)
        draw_skeleton(list, ctx, tile, 28, alpha * (1.0f - in));
    if (qr_size_ <= 0 || in <= 0.01f)
        return;

    const int m = module_px();
    const float mf = static_cast<float>(m);
    const float radius = std::min(28.0f, 3.0f * mf); /* inside the quiet zone */
    list.push_opacity(alpha * in);
    list.shadow({tile.x, tile.y + 24, tile.w, tile.h}, radius, 48, Color::rgb(0x000000, 0.5f));
    list.rounded_rect(tile, radius, kWhite);
    const float ox = tile.x + static_cast<float>(kQrQuiet) * mf;
    const float oy = tile.y + static_cast<float>(kQrQuiet) * mf;
    for (const QrRun &r : runs_)
        list.rounded_rect({ox + static_cast<float>(r.col) * mf, oy + static_cast<float>(r.row) * mf,
                           static_cast<float>(r.len) * mf, mf},
                          0, kInk);
    ui::text(list, ctx.fonts.regular, "Or scan this with your phone", tile.cx(), kExpiryY, 24,
             ctx.palette.text_muted, Align::center);
    list.pop_opacity();
}

/* ---------------------------------------------------------------- messages */

const TwitchLoginScreen::Lines &TwitchLoginScreen::lines(const Context &ctx) const
{
    Lines &l = lines_;
    if (l.fonts == &ctx.fonts && l.rev == rev_)
        return l;
    const ui::Fonts &fonts = ctx.fonts;
    l.fonts = &ctx.fonts;
    l.rev = rev_;
    l.error = fit_lines(fonts.regular, error_.empty() ? std::string("Something went wrong.") : error_, kMessageSize,
                        kMessageW, 3);
    /* A name the fonts can't draw in full (a localised one) reads as the login. */
    const std::string name = drawable(fonts.display, name_);
    l.lead_w = fonts.display.measure(kLoggedInAs, kDoneSize);
    l.name = fonts.display.font->fit(name.empty() || name != name_ ? login_ : name, kDoneSize, kNameW - l.lead_w);
    l.name_w = fonts.display.measure(l.name, kDoneSize);
    return l;
}

void TwitchLoginScreen::draw_error(DrawList &list, const Context &ctx, float alpha) const
{
    if (alpha <= 0.01f)
        return;
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    const Lines &l = lines(ctx);
    list.push_opacity(alpha);
    draw_app_lettering(list, ctx, kMid - 72, kMessageMarkY + 17, 144, ::hui::gfx::mix(p.bg_top, kWhite, 0.16f));
    ui::text(list, fonts.display, "Couldn't log in", kMid, kHeadlineY, kHeadlineSize, p.text, Align::center);
    float y = kMessageY;
    for (const std::string &line : l.error)
    {
        ui::text(list, fonts.regular, line, kMid, y, kMessageSize, p.text_muted, Align::center);
        y += kMessageLead;
    }
    draw_press_hint(list, ctx, kMid, y + 26, ui::Button::cross, "to try again");
    list.pop_opacity();
}

void TwitchLoginScreen::draw_done(DrawList &list, const Context &ctx, float alpha) const
{
    if (alpha <= 0.01f)
        return;
    const ui::Fonts &fonts = ctx.fonts;
    const Palette &p = ctx.palette;
    const Lines &l = lines(ctx);

    list.push_opacity(alpha);
    /* The tick pops in, a beat after the page turns. */
    const float pop = tween::back_out(std::max(0.0f, done_age_ - 0.08f) / 0.45f);
    list.push_transform(0.6f + 0.4f * pop, kMid, kTickY, 0, 0);
    list.glow({kMid - kTickR, kTickY - kTickR, 2 * kTickR, 2 * kTickR}, kTickR, 30, p.accent.with_alpha(0.45f));
    list.circle(kMid, kTickY, kTickR, p.accent);
    const float t = 9.0f;
    list.line(kMid - 26, kTickY + 2, kMid - 8, kTickY + 20, t, kWhite);
    list.line(kMid - 8, kTickY + 20, kMid + 28, kTickY - 18, t, kWhite);
    list.circle(kMid - 8, kTickY + 20, t * 0.5f, kWhite); /* the corner, round */
    list.pop_transform();

    const float x = kMid - (l.lead_w + l.name_w) * 0.5f;
    ui::text(list, fonts.display, kLoggedInAs, x, kDoneY, kDoneSize, p.text_muted);
    ui::text(list, fonts.display, l.name, x + l.lead_w, kDoneY, kDoneSize, p.text);
    list.pop_opacity();
}

void TwitchLoginScreen::draw_checking(DrawList &list, const Context &ctx, float alpha) const
{
    if (alpha <= 0.01f)
        return;
    list.push_opacity(alpha);
    draw_spinner(list, ctx, kMid, kCheckingY, 36, ctx.palette.accent_alt);
    ui::text(list, ctx.fonts.display, "Checking your Twitch login", kMid, kCheckingTextY, 44, ctx.palette.text,
             Align::center);
    list.pop_opacity();
}

/* --------------------------------------------------------------------- draw */

void TwitchLoginScreen::draw(DrawList &list, const Context &ctx) const
{
    draw_backdrop(list, ctx);

    const float head = tween::stagger(age_, 0, 0.06f, 0.5f);
    list.push_transform(1.0f, 0, 0, 0, 16.0f * (1.0f - head));
    draw_header(list, ctx, head * steps_in_.value);
    list.pop_transform();

    const float body = tween::stagger(age_, 1, 0.08f, 0.6f);
    const float steps = body * steps_in_.value;
    /* The steps slide in from below; the QR only fades, so its modules stay
     * on whole pixels. */
    list.push_transform(1.0f, 0, 0, 0, 24.0f * (1.0f - body));
    draw_steps(list, ctx, steps);
    list.pop_transform();
    draw_qr(list, ctx, tween::stagger(age_, 2, 0.08f, 0.6f) * steps_in_.value);
    draw_checking(list, ctx, body * checking_in_.value);
    draw_error(list, ctx, body * error_in_.value);
    draw_done(list, ctx, body * done_in_.value);

    ui::Hint hints[2];
    int h = 0;
    if (state_ == EVO_HUI_TWITCH_LOGIN_ERROR)
        hints[h++] = {ui::Button::cross, "Try again"};
    if (state_ != EVO_HUI_TWITCH_LOGIN_DONE)
        hints[h++] = {ui::Button::circle, "Back"};
    draw_twitch_hints(list, ctx, hints, h, tween::stagger(age_, 6, 0.08f, 0.5f));
}

/* -------------------------------------------------------------------- focus */

bool TwitchLoginScreen::focus(FocusInfo *out) const
{
    /* No cursor: Cross and Circle act on the page as a whole. */
    out->focusable.clear();
    return false;
}

} // namespace evo::kit
