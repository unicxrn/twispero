/*
 * evo_theme - the app's colours. There is one theme, TWITCH (EVO's other
 * built-in themes went with the Settings row that picked them).
 */
#include "evo_theme.h"

/* ------------------------------------------------------------------------ */
/* Design rules:
 *   - the page background is a near-black vertical gradient, never flat
 *   - cards sit one step above it and carry their own subtle gradient
 *   - resting borders are low-contrast hairlines; the accent is reserved for
 *     the selected row, icons and rails, so the eye has one thing to follow
 *   - text is three weights: primary, secondary, muted
 * ------------------------------------------------------------------------ */

/*
 * Twitch's own dark palette: near-black background, two raised-surface greys
 * (resting card, then a lighter one for the focused card - the same role
 * border_sel/accent play elsewhere, just carried into the fill too), the brand
 * purple as the accent and a lighter violet as its secondary. "Live" gets
 * Twitch's own red rather than a tint of the accent.
 */
static const evo_theme k_twitch = {
    .name = "TWITCH",
    .bg_top          = EVO_RGBA(0x0E, 0x0E, 0x10, 255),
    .bg_bottom       = EVO_RGBA(0x07, 0x07, 0x08, 255),
    .scrim           = EVO_RGBA(0x00, 0x00, 0x00, 170),
    .surface         = EVO_RGBA(0x18, 0x18, 0x1B, 240),
    .surface_alt     = EVO_RGBA(0x13, 0x13, 0x16, 240),
    .surface_sel     = EVO_RGBA(0x1F, 0x1F, 0x23, 250),
    .surface_sel_alt = EVO_RGBA(0x1A, 0x1A, 0x1E, 250),
    .border          = EVO_RGBA(0x6E, 0x5A, 0x8C, 51),
    .border_sel      = EVO_RGBA(0x91, 0x46, 0xFF, 220),
    .shadow          = EVO_RGBA(0x00, 0x00, 0x00, 150),
    .accent          = EVO_RGBA(0x91, 0x46, 0xFF, 255),
    .accent_soft     = EVO_RGBA(0x91, 0x46, 0xFF, 55),
    .accent_alt      = EVO_RGBA(0xBF, 0x94, 0xFF, 255),
    .danger          = EVO_RGBA(0xEB, 0x04, 0x00, 255),
    .text_primary    = EVO_RGBA(0xEF, 0xEF, 0xF1, 255),
    .text_secondary  = EVO_RGBA(0xAD, 0xAD, 0xB8, 255),
    .text_muted      = EVO_RGBA(0x6B, 0x6B, 0x74, 255),
    .radius = 12, .border_px = 1, .shadow_px = 12, .rail_px = 4,
    .row_h = 84, .row_gap = 10, .pad_x = 24,
};

const evo_theme *evo_theme_current(void)
{
    return &k_twitch;
}
