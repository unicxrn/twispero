/*
 * evo_hui_twitch_chat.cpp - Twitch chat in the player, on ps5-homebrew-ui.
 *
 * Side mode: the video sits in the left EVO_CHAT_VIDEO_W columns
 * (twitch/include/evo_twitch_chat_layout.h) and chat fills the panel to its
 * right in Twitch's own dark chat colours, under a "Stream chat" header.
 * Overlay mode: the newest lines on a dim, rounded column over the right of
 * the video, as tall as what it holds until it is full.
 *
 * Lines stack up from the bottom, newest last: badges, the name in its
 * colour, then the text and the emotes, wrapped at spaces. A reply has
 * "Replying to @name" over it in small muted text, and a line that mentions
 * the logged-in user sits on a purple tint. Each line is laid out once
 * (ChatLayout) and kept while it stays in the params and the text size
 * doesn't change.
 *
 * The player doesn't clear the frame between video frames, so side mode
 * paints its panel and the bars around the video, opaque, on every draw.
 */
#include "evo_hui_twitch.hpp"

#include "evo_twitch_chat_layout.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace evo::kit
{

using ::hui::gfx::Align;
using ::hui::gfx::Color;
using ::hui::gfx::DrawList;
using ::hui::gfx::Rect;
namespace ui = ::hui::ui;

namespace
{

/* Twitch's dark chat. */
const Color kPanel = Color::rgb(0x18181b);
const Color kDivider = Color::rgb(0x2f2f35);
const Color kBody = Color::rgb(0xefeff1);
const Color kMuted = Color::rgb(0xadadb8);
const Color kPurple = Color::rgb(0x9147ff);
const Color kBlack = Color::rgb(0x000000);
const Color kWhite = Color::rgb(0xffffff);

/* Inter's capital height as a share of the size: rows centre the capitals,
 * and pictures sit on the same centre. */
constexpr float kCapHeight = 0.727f;

/* The sizes of one mode, at one text size. */
struct Metrics
{
    int mode;                       /* EVO_HUI_TWITCH_CHAT_SIDE or _OVERLAY */
    float size, lead;               /* message text and its row pitch */
    float status_size, status_lead; /* the app's own lines, and a reply's "Replying to" */
    float badge, emote;             /* picture heights */
    float gap;                      /* between two lines */
    float pad;                      /* inside the panel */
    std::uint32_t muted;            /* status lines and notices; lighter over the video */
    int text_size = EVO_HUI_TWITCH_CHAT_TEXT_MEDIUM;
};
constexpr Metrics kSide{EVO_HUI_TWITCH_CHAT_SIDE, 24.0f, 34.0f, 20.0f, 30.0f, 24.0f, 32.0f, 12.0f, 24.0f, 0xadadb8};
constexpr Metrics kOverlay{EVO_HUI_TWITCH_CHAT_OVERLAY, 22.0f, 31.0f, 18.0f, 27.0f, 22.0f, 28.0f, 10.0f, 16.0f,
                           0xc8c8d0};

/* The Small and Large text sizes against Medium (the sizes above). The panel
 * and its padding stay; text, pictures and the gaps between lines scale. */
constexpr float kSmallScale = 0.875f;
constexpr float kLargeScale = 1.25f;

Metrics metrics(int mode, int text_size)
{
    Metrics m = mode == EVO_HUI_TWITCH_CHAT_OVERLAY ? kOverlay : kSide;
    m.text_size = text_size;
    const float k = text_size == EVO_HUI_TWITCH_CHAT_TEXT_SMALL   ? kSmallScale
                    : text_size == EVO_HUI_TWITCH_CHAT_TEXT_LARGE ? kLargeScale
                                                                  : 1.0f;
    if (k == 1.0f)
        return m;
    for (float *v : {&m.size, &m.lead, &m.status_size, &m.status_lead, &m.badge, &m.emote, &m.gap})
        *v = std::round(*v * k);
    return m;
}

/* Side mode: the panel right of the video and its header. */
constexpr float kSideX = EVO_CHAT_VIDEO_W;
constexpr float kHeaderH = 64.0f;
constexpr float kTitleSize = 24.0f;
constexpr float kChannelSize = 20.0f;
/* Rows fade out over this much as they near the top (the header in side
 * mode, the column's edge in overlay mode), so the oldest line shown, cut
 * there, goes soft instead of showing half its letters. */
constexpr float kFadeH = 48.0f;
/* How far a shape that meets the screen's edge runs past it, so it has no
 * soft edge there. */
constexpr float kBleed = 8.0f;

/* Overlay mode: the column grows up from its bottom to this box. */
const Rect kOverlayBox{1488.0f, 120.0f, 400.0f, 840.0f};
constexpr float kOverlayRadius = 16.0f;
constexpr float kOverlayAlpha = 0.65f;

/* A notice's card: the bar down its left, the text after it. */
constexpr float kBarW = 3.0f;
constexpr float kCardIndent = 16.0f; /* the text from the card's left edge */
constexpr float kCardPadR = 12.0f;
constexpr float kCardPadY = 8.0f;

/* A reply's "Replying to @name" row: this much of the status row pitch. */
constexpr float kReplyLead = 0.9f;

/* A mention's tint, and how far it reaches past the line's text into the
 * padding; up and down it takes all but a pixel of half the gap between
 * lines, so two mentions in a row stay two. */
constexpr float kMentionAlpha = 0.24f;
constexpr float kMentionPadX = 10.0f;

/* Between two badges, and from the last badge to the name. */
constexpr float kBadgeGap = 4.0f;
constexpr float kNameGap = 6.0f;
/* Room kept for the name beside the last badge, in text sizes: with less,
 * the badge goes to the next row with the name. */
constexpr float kNameKeep = 4.0f;

bool same(const Color &a, const Color &b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

/* Places a line's pieces left to right and starts a row when the next one
 * would not fit. Words never break, except one wider than a whole row: that
 * one is cut short, as fit_lines() does. Words in the same font and colour
 * on a row become one text run. */
class Flow
{
  public:
    Flow(const ui::Fonts &fonts, std::vector<ChatPiece> &out, float indent, float top, float width, float size,
         float lead)
        : fonts_(fonts), out_(out), indent_(indent), top_(top), width_(width), size_(size), lead_(lead)
    {
        space_w_ = fonts.regular.measure(" ", size);
        base_ = std::round((lead + kCapHeight * size) * 0.5f);
    }

    /* Room before the next piece, unless it starts a row. */
    void gap(float px) { pending_ = std::max(pending_, px); }
    void space() { gap(space_w_); }

    void newline()
    {
        if (!used_)
            return;
        ++row_;
        x_ = 0.0f;
        pending_ = 0.0f;
        open_ = -1;
        used_ = false;
    }

    /* `raw` as the params have it: its spaces, and characters the font
     * lacks (drawable() drops them; a run of them still parts two words). */
    void text(const std::string &raw, bool semibold, Color color)
    {
        if (raw.empty())
            return;
        const std::string s = drawable(font(semibold), raw);
        if (s.empty())
        {
            space();
            return;
        }
        if (static_cast<unsigned char>(raw.front()) <= 0x20)
            space();
        for (std::size_t i = 0; i < s.size();)
        {
            std::size_t j = s.find(' ', i);
            if (j == std::string::npos)
                j = s.size();
            word(s.substr(i, j - i), semibold, color, 0.0f);
            if (j < s.size())
                space();
            i = j + 1;
        }
        if (static_cast<unsigned char>(raw.back()) <= 0x20)
            space();
    }

    /* One unbreakable piece of text. `keep` is the width of what must stay
     * on the same row after it (the colon after a name). With `stay` it goes
     * on this row whatever its width (a name beside its badges), cut to the
     * room there is. */
    void word(const std::string &w, bool semibold, Color color, float keep, bool stay = false)
    {
        if (w.empty())
            return;
        const ui::FontRef &f = font(semibold);
        if (open_ >= 0)
        {
            ChatPiece &run = out_[static_cast<std::size_t>(open_)];
            if (run.semibold == semibold && same(run.color, color))
            {
                std::string joined = run.str;
                if (pending_ > 0.0f)
                    joined += ' ';
                joined += w;
                const float right = run.x - indent_ + f.measure(joined, size_);
                if (right + keep <= width_)
                {
                    run.str = std::move(joined);
                    x_ = right;
                    pending_ = 0.0f;
                    return;
                }
                newline();
            }
        }
        float ww = f.measure(w, size_);
        float sx = stay ? here() : start(ww + keep);
        ChatPiece p;
        p.kind = ChatPiece::text;
        p.semibold = semibold;
        p.size = size_;
        p.color = color;
        p.str = w;
        if (sx + ww + keep > width_)
        {
            p.str = f.font->fit(w, size_, width_ - sx - keep);
            ww = f.measure(p.str, size_);
        }
        p.x = indent_ + sx;
        p.y = row_top() + base_;
        out_.push_back(std::move(p));
        open_ = static_cast<int>(out_.size()) - 1;
        x_ = sx + ww;
        used_ = true;
    }

    /* A badge or an emote, `w` x `h`, centred on the row; `keep` as for
     * word(). */
    void picture(ChatPiece::Kind kind, int ref, float w, float h, float keep = 0.0f)
    {
        if (w > width_)
        {
            h *= width_ / w;
            w = width_;
        }
        const float sx = start(std::min(w + keep, width_));
        ChatPiece p;
        p.kind = kind;
        p.ref = ref;
        p.x = indent_ + sx;
        p.y = row_top() + std::round((lead_ - h) * 0.5f);
        p.w = w;
        p.h = h;
        out_.push_back(std::move(p));
        open_ = -1;
        x_ = sx + w;
        used_ = true;
    }

    int rows() const { return row_ + (used_ ? 1 : 0); }

  private:
    const ui::FontRef &font(bool semibold) const { return semibold ? fonts_.semibold : fonts_.regular; }
    float row_top() const { return top_ + lead_ * static_cast<float>(row_); }

    /* Where a piece `w` wide starts: after the pending gap, or at the start
     * of the next row when it would not fit on this one. */
    float start(float w)
    {
        float sx = x_ > 0.0f ? x_ + pending_ : 0.0f;
        if (sx + w > width_ && x_ > 0.0f)
        {
            newline();
            sx = 0.0f;
        }
        pending_ = 0.0f;
        return sx;
    }

    /* Where the next piece starts on this row, fitting or not. */
    float here()
    {
        const float sx = x_ > 0.0f ? x_ + pending_ : 0.0f;
        pending_ = 0.0f;
        return sx;
    }

    const ui::Fonts &fonts_;
    std::vector<ChatPiece> &out_;
    float indent_, top_, width_, size_, lead_;
    float space_w_ = 0.0f, base_ = 0.0f;
    float x_ = 0.0f, pending_ = 0.0f;
    int row_ = 0;
    int open_ = -1; /* the text run the next word may join */
    bool used_ = false;
};

/* Which emote pictures have arrived, and their shapes: a line is laid out
 * again when this changes. */
std::uint64_t picture_key(const ChatLine &line)
{
    std::uint64_t h = 1469598103934665603ull;
    for (const ChatSegment &s : line.segments)
    {
        if (!s.emote)
            continue;
        const std::uint64_t v = s.pic.px ? 1u + static_cast<std::uint64_t>(s.pic.w) * 65536u +
                                               static_cast<std::uint64_t>(s.pic.h)
                                         : 0u;
        h = (h ^ v) * 1099511628211ull;
    }
    return h;
}

void lay_out(const Context &ctx, const ChatLine &line, const Metrics &m, float width, ChatLayout &out)
{
    const ui::Fonts &fonts = ctx.fonts;
    out.pieces.clear();
    out.card = false;
    out.height = 0.0f;

    if (line.kind == EVO_HUI_TWITCH_CHAT_STATUS)
    {
        Flow flow(fonts, out.pieces, 0.0f, 0.0f, width, m.status_size, m.status_lead);
        flow.text(line.system_text, false, Color::rgb(m.muted));
        out.height = m.status_lead * static_cast<float>(flow.rows());
        return;
    }

    const bool notice = line.kind == EVO_HUI_TWITCH_CHAT_NOTICE;
    const float indent = notice ? kCardIndent : 0.0f;
    const float pad_y = notice ? kCardPadY : 0.0f;
    const float text_w = width - indent - (notice ? kCardPadR : 0.0f);

    /* A reply: who it answers, on one row of its own over it, small and
     * muted; cut short when the name is long. The login stands in for a
     * name the fonts can't draw. */
    float top = pad_y;
    if (!notice)
    {
        std::string who = drawable(fonts.regular, line.reply_to);
        if (who.size() != line.reply_to.size())
            who = drawable(fonts.regular, line.reply_login);
        if (!who.empty())
        {
            const float lead = std::round(m.status_lead * kReplyLead);
            ChatPiece p;
            p.kind = ChatPiece::text;
            p.size = m.status_size;
            p.color = Color::rgb(m.muted);
            p.str = fonts.regular.font->fit("Replying to @" + who, m.status_size, text_w);
            p.x = indent;
            p.y = top + std::round((lead + kCapHeight * m.status_size) * 0.5f);
            out.pieces.push_back(std::move(p));
            top += lead;
        }
    }

    Flow flow(fonts, out.pieces, indent, top, text_w, m.size, m.lead);
    if (notice)
    {
        /* Twitch's sentence about it, then the viewer's own message under it,
         * as an ordinary line. */
        out.card = true;
        flow.text(line.system_text, false, Color::rgb(m.muted));
        flow.newline();
    }
    if (!notice || !line.segments.empty())
    {
        const bool action = line.kind == EVO_HUI_TWITCH_CHAT_ACTION;
        const Color name_color = Color::rgb(line.color & 0xffffffu);
        /* The login stands in for a name the fonts can't draw (Korean, say);
         * with neither, the line starts at its text. */
        std::string name = drawable(fonts.semibold, line.name);
        if (name.empty())
            name = drawable(fonts.semibold, line.login);
        /* "/me" lines have no colon: the text follows the name. */
        const float colon_w = name.empty() || action ? 0.0f : fonts.regular.measure(":", m.size);
        const float name_w = name.empty() ? 0.0f : fonts.semibold.measure(name, m.size);
        /* The last badge keeps room for the name (some of it, cut if long)
         * beside it, or goes to the next row with it. */
        const float name_keep =
            name.empty() ? 0.0f : kNameGap + std::min(name_w, kNameKeep * m.size) + colon_w;
        for (std::size_t i = 0; i < line.badges.size(); ++i)
        {
            flow.gap(kBadgeGap);
            const bool last = i + 1 == line.badges.size();
            flow.picture(ChatPiece::badge, static_cast<int>(i), m.badge, m.badge, last ? name_keep : 0.0f);
        }
        if (!name.empty())
        {
            flow.gap(kNameGap);
            flow.word(name, true, name_color, colon_w, true);
            if (!action)
                flow.word(":", false, kBody, 0.0f, true);
            flow.space();
        }
        const Color ink = action ? name_color : kBody;
        for (std::size_t i = 0; i < line.segments.size(); ++i)
        {
            const ChatSegment &s = line.segments[i];
            if (s.emote && s.pic.px)
            {
                const float w = m.emote * static_cast<float>(s.pic.w) / static_cast<float>(s.pic.h);
                flow.picture(ChatPiece::emote, static_cast<int>(i), std::round(w), m.emote);
            }
            else
            {
                /* An emote whose picture is on its way shows its name. */
                flow.text(s.text, false, s.emote ? kBody : ink);
            }
        }
    }
    const float rows = static_cast<float>(flow.rows());
    out.height = rows > 0.0f ? top + pad_y + m.lead * rows : 0.0f;
}

/* A line laid out for mode m, whose rows are always `width` wide. */
const ChatLayout &layout_of(const Context &ctx, const ChatLine &line, const Metrics &m, float width)
{
    ChatLayout &l = line.layout;
    const std::uint64_t pictures = picture_key(line);
    if (l.fonts != &ctx.fonts || l.mode != m.mode || l.text_size != m.text_size || l.pictures != pictures)
    {
        lay_out(ctx, line, m, width, l);
        l.fonts = &ctx.fonts;
        l.mode = m.mode;
        l.text_size = m.text_size;
        l.pictures = pictures;
    }
    return l;
}

/* A w x h picture as large as it fits in r, centred. */
Rect contain(const Rect &r, int w, int h)
{
    if (w <= 0 || h <= 0 || w == h)
        return r;
    const float a = static_cast<float>(w) / static_cast<float>(h);
    if (a > r.w / r.h)
    {
        const float fh = r.w / a;
        return {r.x, r.y + (r.h - fh) * 0.5f, r.w, fh};
    }
    const float fw = r.h * a;
    return {r.x + (r.w - fw) * 0.5f, r.y, fw, r.h};
}

/* Rows fading out towards `top` over the `len` pixels under it, by where
 * their middle is; len 0: nothing fades. */
struct Fade
{
    float top = 0.0f, len = 0.0f;
    float at(float y) const { return len > 0.0f ? std::clamp((y - top) / len, 0.0f, 1.0f) : 1.0f; }
};

/* r filled with `color` under `fade`: a gradient across the fade, solid
 * below it, nothing above it. */
void faded_rect(DrawList &list, const Rect &r, Color color, const Fade &fade)
{
    const float end = fade.top + fade.len;
    if (fade.len <= 0.0f || r.y >= end)
    {
        list.rounded_rect(r, 0.0f, color);
        return;
    }
    const float y0 = std::max(r.y, fade.top), y1 = std::min(r.y + r.h, end);
    if (y1 > y0)
        list.gradient_rect({r.x, y0, r.w, y1 - y0}, 0.0f, color.with_alpha(fade.at(y0)),
                           color.with_alpha(fade.at(y1))); /* with_alpha() multiplies */
    if (r.y + r.h > end)
        list.rounded_rect({r.x, end, r.w, r.y + r.h - end}, 0.0f, color);
}

/* A laid-out line with its top left at (x, y), `width` wide, `gap` above
 * and under it. */
void draw_line(DrawList &list, const Context &ctx, const ChatLine &line, float x, float y, float width, float gap,
               const Fade &fade)
{
    const ui::Fonts &fonts = ctx.fonts;
    const ChatLayout &l = line.layout;
    if (line.mention && l.height > 0.0f)
    {
        const float pad_y = std::max(0.0f, std::floor(gap * 0.5f) - 1.0f);
        const Rect tint{x - kMentionPadX, y - pad_y, width + 2.0f * kMentionPadX, l.height + 2.0f * pad_y};
        faded_rect(list, tint, kPurple.with_alpha(kMentionAlpha), fade);
    }
    if (line.selected && l.height > 0.0f)
    {
        const float pad_y = std::max(0.0f, std::floor(gap * 0.5f) - 1.0f);
        const Rect tint{x - kMentionPadX, y - pad_y, width + 2.0f * kMentionPadX, l.height + 2.0f * pad_y};
        faded_rect(list, tint, kWhite.with_alpha(0.10f), fade);
        faded_rect(list, {x - kMentionPadX, y - pad_y, kBarW, l.height + 2.0f * pad_y}, kWhite, fade);
    }
    if (l.card)
    {
        faded_rect(list, {x, y, width, l.height}, kWhite.with_alpha(0.05f), fade);
        faded_rect(list, {x, y, kBarW, l.height}, kPurple, fade);
    }
    for (const ChatPiece &p : l.pieces)
    {
        if (p.kind == ChatPiece::text)
        {
            const float a = fade.at(y + p.y - p.size * kCapHeight * 0.5f);
            if (a > 0.0f)
                ui::text(list, p.semibold ? fonts.semibold : fonts.regular, p.str, x + p.x, y + p.y, p.size,
                         p.color.with_alpha(a));
            continue;
        }
        const Rect r{x + p.x, y + p.y, p.w, p.h};
        const float a = fade.at(r.cy());
        if (a <= 0.0f)
            continue;
        if (p.kind == ChatPiece::badge)
        {
            const TwitchPicture &pic = line.badges[static_cast<std::size_t>(p.ref)];
            const std::uint32_t tex = twitch_texture(ctx, pic);
            if (tex)
                list.image(tex, contain(r, pic.w, pic.h), ::hui::gfx::kFullUv, kWhite.with_alpha(a));
            else
                list.rounded_rect(r, 4.0f, kWhite.with_alpha(0.08f * a)); /* its place, while it loads */
        }
        else
        {
            const std::uint32_t tex = twitch_texture(ctx, line.segments[static_cast<std::size_t>(p.ref)].pic);
            if (tex)
                list.image(tex, r, ::hui::gfx::kFullUv, kWhite.with_alpha(a));
        }
    }
}

/* How many of the newest lines it takes to fill `room` (the last of them
 * may stick out above it), and how tall they are together, in *used. */
std::size_t newest_lines(const Context &ctx, const std::vector<ChatLine> &lines, const Metrics &m, float width,
                         float room, float *used)
{
    float h = 0.0f;
    std::size_t count = 0;
    for (auto it = lines.rbegin(); it != lines.rend() && h < room; ++it, ++count)
    {
        const ChatLayout &l = layout_of(ctx, *it, m, width);
        if (l.height > 0.0f)
            h += (h > 0.0f ? m.gap : 0.0f) + l.height;
    }
    *used = h;
    return count;
}

/* The newest `count` lines stacked up from `bottom`, newest last. */
void draw_newest(DrawList &list, const Context &ctx, const std::vector<ChatLine> &lines, std::size_t count,
                 const Metrics &m, float x, float bottom, float width, const Fade &fade)
{
    float y = bottom;
    for (std::size_t i = 0; i < count; ++i)
    {
        const ChatLine &line = lines[lines.size() - 1 - i];
        if (line.layout.height <= 0.0f)
            continue;
        y -= line.layout.height;
        draw_line(list, ctx, line, x, y, width, m.gap, fade);
        y -= m.gap;
    }
}

/* Opaque black over what the video `v` leaves uncovered of `box`. The
 * edges against the picture are exact; the others run `bleed` past the box,
 * over what is no picture either, so no soft edge is left to collect old
 * pixels. */
void paint_uncovered(DrawList &list, const Rect &box, const Rect &v, float bleed)
{
    const Rect b{box.x - bleed, box.y - bleed, box.w + 2.0f * bleed, box.h + 2.0f * bleed};
    const float right = b.x + b.w, bottom = b.y + b.h;
    const float x0 = std::clamp(v.x, b.x, right), x1 = std::clamp(v.x + v.w, b.x, right);
    const float y0 = std::clamp(v.y, b.y, bottom), y1 = std::clamp(v.y + v.h, b.y, bottom);
    if (x1 <= x0 || y1 <= y0)
    {
        list.rounded_rect(b, 0.0f, kBlack);
        return;
    }
    if (y0 > b.y)
        list.rounded_rect({b.x, b.y, b.w, y0 - b.y}, 0.0f, kBlack);
    if (y1 < bottom)
        list.rounded_rect({b.x, y1, b.w, bottom - y1}, 0.0f, kBlack);
    if (x0 > b.x)
        list.rounded_rect({b.x, y0, x0 - b.x, y1 - y0}, 0.0f, kBlack);
    if (x1 < right)
        list.rounded_rect({x1, y0, right - x1, y1 - y0}, 0.0f, kBlack);
}

std::string str(const char *s)
{
    return std::string(s ? s : "");
}

void copy_line(ChatLine &out, const evo_hui_twitch_chat_line_t &in)
{
    out.serial = in.serial;
    out.kind = in.kind;
    out.name = str(in.name);
    out.login = str(in.login);
    out.system_text = str(in.system_text);
    out.reply_to = str(in.reply_to);
    out.reply_login = str(in.reply_login);
    out.mention = in.mention != 0;
    out.selected = in.selected != 0;
    out.color = in.color;
    out.badges.clear();
    if (in.badges)
        for (int i = 0; i < in.badge_count; ++i)
            out.badges.push_back(twitch_picture(in.badges[i]));
    out.segments.clear();
    if (in.segments)
        for (int i = 0; i < in.segment_count; ++i)
        {
            const evo_hui_twitch_chat_segment_t &s = in.segments[i];
            ChatSegment seg;
            seg.emote = s.emote != 0;
            seg.text = str(s.text);
            if (seg.emote)
                seg.pic = twitch_picture(s.pic);
            out.segments.push_back(std::move(seg));
        }
}

/* The same line as before, with this frame's pictures. False when it isn't
 * shaped like the line kept under its serial (then it is copied afresh). */
bool refresh_line(ChatLine &line, const evo_hui_twitch_chat_line_t &in)
{
    const std::size_t badges = in.badges && in.badge_count > 0 ? static_cast<std::size_t>(in.badge_count) : 0;
    const std::size_t segments =
        in.segments && in.segment_count > 0 ? static_cast<std::size_t>(in.segment_count) : 0;
    if (line.kind != in.kind || line.badges.size() != badges || line.segments.size() != segments)
        return false;
    line.mention = in.mention != 0; /* the login can change while the line stays */
    line.selected = in.selected != 0;
    for (std::size_t i = 0; i < badges; ++i)
        line.badges[i] = twitch_picture(in.badges[i]);
    for (std::size_t i = 0; i < segments; ++i)
        if (line.segments[i].emote)
            line.segments[i].pic = twitch_picture(in.segments[i].pic);
    return true;
}

} // namespace

/* -------------------------------------------------------------------- state */

void TwitchChatScreen::set(const evo_hui_twitch_chat_params_t &p)
{
    mode_ = p.mode == EVO_HUI_TWITCH_CHAT_OVERLAY ? EVO_HUI_TWITCH_CHAT_OVERLAY : EVO_HUI_TWITCH_CHAT_SIDE;
    text_size_ = p.text_size == EVO_HUI_TWITCH_CHAT_TEXT_SMALL || p.text_size == EVO_HUI_TWITCH_CHAT_TEXT_LARGE
                     ? p.text_size
                     : EVO_HUI_TWITCH_CHAT_TEXT_MEDIUM;
    channel_.assign(p.channel ? p.channel : "");
    paused_ = p.paused != 0;

    /* The video's rect, kept to the screen (a zoomed picture may start off
     * it). Anything not finite, empty or off the screen counts as unknown. */
    video_ = Rect{};
    const float vx = p.video_x, vy = p.video_y, vw = p.video_w, vh = p.video_h;
    if (std::isfinite(vx) && std::isfinite(vy) && std::isfinite(vw) && std::isfinite(vh) && vw > 0.0f && vh > 0.0f)
    {
        const float x0 = std::max(vx, 0.0f), y0 = std::max(vy, 0.0f);
        const float x1 = std::min(vx + vw, EVO_CHAT_CANVAS_W), y1 = std::min(vy + vh, EVO_CHAT_CANVAS_H);
        if (x1 > x0 && y1 > y0)
            video_ = {x0, y0, x1 - x0, y1 - y0};
    }

    /* Both lists run oldest first with rising serials: walk them together,
     * keep the lines still there (with their layouts), copy in the new ones
     * and let go of the rest. */
    const int n = p.lines && p.line_count > 0 ? p.line_count : 0;
    merged_.clear();
    merged_.reserve(static_cast<std::size_t>(n));
    std::size_t old = 0;
    for (int i = 0; i < n; ++i)
    {
        const evo_hui_twitch_chat_line_t &in = p.lines[i];
        while (old < lines_.size() && lines_[old].serial < in.serial)
            ++old;
        if (old < lines_.size() && lines_[old].serial == in.serial && refresh_line(lines_[old], in))
        {
            merged_.push_back(std::move(lines_[old]));
            ++old;
            continue;
        }
        merged_.emplace_back();
        copy_line(merged_.back(), in);
    }
    lines_.swap(merged_);
    merged_.clear();
}

void TwitchChatScreen::update(float dt)
{
    (void)dt; /* nothing moves: lines appear where they belong */
}

bool TwitchChatScreen::focus(FocusInfo *out) const
{
    out->focusable.clear();
    return false;
}

/* --------------------------------------------------------------------- draw */

void TwitchChatScreen::draw(DrawList &list, const Context &ctx) const
{
    if (mode_ == EVO_HUI_TWITCH_CHAT_OVERLAY)
        draw_overlay(list, ctx);
    else
        draw_side(list, ctx);
}

void TwitchChatScreen::draw_side(DrawList &list, const Context &ctx) const
{
    const ui::Fonts &fonts = ctx.fonts;
    const Metrics m = metrics(EVO_HUI_TWITCH_CHAT_SIDE, text_size_);
    const float screen_h = EVO_CHAT_CANVAS_H;

    /* Black where the video leaves its column uncovered: above and under
     * it, or beside it. */
    Rect video = video_;
    if (video.w <= 0.0f)
        evo_chat_side_video(16.0f / 9.0f, &video.x, &video.y, &video.w, &video.h);
    paint_uncovered(list, {0.0f, 0.0f, kSideX, screen_h}, video, kBleed);

    const Rect panel{kSideX, 0.0f, EVO_CHAT_SIDE_W, screen_h};
    list.rounded_rect({panel.x, -kBleed, panel.w + kBleed, panel.h + 2.0f * kBleed}, 0.0f, kPanel);

    /* The header: what this is, and whose. */
    const float left = panel.x + m.pad;
    const float right = panel.x + panel.w - m.pad;
    const float base = std::round((kHeaderH + kCapHeight * kTitleSize) * 0.5f);
    const float title_w = ui::text(list, fonts.semibold, paused_ ? "Chat paused" : "Stream chat", left, base,
                                   kTitleSize, kBody);
    const std::string &channel = channel_fit_.get(fonts.regular, channel_, kChannelSize, right - left - title_w - 24.0f);
    ui::text(list, fonts.regular, channel, right, base, kChannelSize, kMuted, Align::right);
    list.rounded_rect({panel.x, kHeaderH - 1.0f, panel.w + kBleed, 1.0f}, 0.0f, kDivider);

    /* The lines, newest at the bottom, up to the header. When they don't
     * all fit, the oldest one shown is cut there and fades out under it. */
    const float top = kHeaderH;
    const float bottom = panel.h - m.pad;
    const float width = right - left;
    float used = 0.0f;
    const std::size_t count = newest_lines(ctx, lines_, m, width, bottom - top, &used);
    Fade fade;
    if (used > bottom - top)
    {
        fade.top = top;
        fade.len = kFadeH;
    }
    list.push_clip({panel.x, top, panel.w, panel.h - top});
    draw_newest(list, ctx, lines_, count, m, left, bottom, width, fade);
    list.pop_clip();
}

void TwitchChatScreen::draw_overlay(DrawList &list, const Context &ctx) const
{
    const Metrics m = metrics(EVO_HUI_TWITCH_CHAT_OVERLAY, text_size_);
    const Rect box = kOverlayBox;
    const float width = box.w - 2.0f * m.pad;
    const float room = box.h - 2.0f * m.pad;

    /* Black where the video doesn't reach into the box (a 4:3 picture's
     * bars), every frame: the column's size changes, and nothing else
     * repaints there. Unknown: the video fills the screen. The edges only
     * bleed a pixel: the OSD may have drawn just outside the box. */
    if (video_.w > 0.0f)
        paint_uncovered(list, box, video_, 1.0f);

    /* As tall as its lines, up to the box. Once full it stays full, and the
     * oldest line shown fades out under its top edge, so the edge never
     * jumps as lines of other heights come and go. */
    float used = 0.0f;
    const std::size_t count = newest_lines(ctx, lines_, m, width, room, &used);
    if (used <= 0.0f)
        return;
    const bool full = used > room;
    const float h = std::min(used, room) + 2.0f * m.pad;
    const Rect panel{box.x, box.y + box.h - h, box.w, h};
    list.rounded_rect(panel, kOverlayRadius, kBlack.with_alpha(kOverlayAlpha));

    Fade fade;
    if (full)
    {
        fade.top = panel.y + m.pad * 0.5f;
        fade.len = kFadeH;
        list.push_clip(panel);
    }
    draw_newest(list, ctx, lines_, count, m, panel.x + m.pad, panel.y + panel.h - m.pad, width, fade);
    if (full)
        list.pop_clip();
}

} // namespace evo::kit
