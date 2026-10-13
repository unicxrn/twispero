#ifndef EVO_TWITCH_CHAT_LAYOUT_H
#define EVO_TWITCH_CHAT_LAYOUT_H
/* Where chat goes, on the kit's 1920 x 1080 canvas. Side mode: the video is
 * fitted into the left EVO_CHAT_VIDEO_W columns, keeping its aspect, and
 * chat fills the column to its right. */
#define EVO_CHAT_CANVAS_W 1920.0f
#define EVO_CHAT_CANVAS_H 1080.0f
#define EVO_CHAT_SIDE_W   440.0f
#define EVO_CHAT_VIDEO_W  (EVO_CHAT_CANVAS_W - EVO_CHAT_SIDE_W)

/* The video's box in side mode as fractions of the screen, for
 * evo_agc_runtime_set_video_rect(). */
static inline void evo_chat_side_box(float* x, float* y, float* w, float* h) {
    *x = 0.0f; *y = 0.0f; *w = EVO_CHAT_VIDEO_W / EVO_CHAT_CANVAS_W; *h = 1.0f;
}

/* Where a video of aspect (w / h; 0 = unknown, taken as 16:9) lands inside
 * the side box, in canvas units: fitted and centred, as the runtime does it.
 * Aspects outside 0.2 .. 5 are taken as the nearest end of that range. */
static inline void evo_chat_side_video(float aspect, float* x, float* y, float* w, float* h) {
    float a = aspect > 0.0f ? aspect : 16.0f / 9.0f; /* NaN too */
    if (a < 0.2f) a = 0.2f;
    if (a > 5.0f) a = 5.0f;
    float bw = EVO_CHAT_VIDEO_W, bh = EVO_CHAT_CANVAS_H;
    if (bw / bh > a) { *h = bh; *w = bh * a; } else { *w = bw; *h = bw / a; }
    *x = (bw - *w) * 0.5f; *y = (bh - *h) * 0.5f;
}
#endif
