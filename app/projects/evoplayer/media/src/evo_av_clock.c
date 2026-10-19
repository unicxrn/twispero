/*
 * evo_av_clock.c - see evo_av_clock.h.
 *
 * Positions are kept as a base PTS plus a sample count rather than as a
 * running microsecond sum: one block is 42666.67 us, so adding whole
 * microseconds per block would drift by a third of one every block.
 */
#include "evo_av_clock.h"

#include <stddef.h>

static int64_t samples_us(int64_t samples, int rate)
{
    if (rate <= 0)
        return 0;
    return (samples * 1000000 + rate / 2) / rate;
}

/* ------------------------------------------------------------- decode side */

void evo_apts_reset(evo_apts_writer *w, int rate)
{
    if (!w)
        return;
    w->rate = rate > 0 ? rate : 48000;
    w->base_us = EVO_AV_NOPTS;
    w->since = 0;
    w->block_us = EVO_AV_NOPTS;
}

void evo_apts_frame(evo_apts_writer *w, int64_t pts_us)
{
    if (!w || pts_us == EVO_AV_NOPTS)
        return; /* no PTS: carry on from the end of the frame before */
    w->base_us = pts_us;
    w->since = 0;
}

static int64_t head_us(const evo_apts_writer *w)
{
    if (w->base_us == EVO_AV_NOPTS)
        return EVO_AV_NOPTS;
    return w->base_us + samples_us(w->since, w->rate);
}

void evo_apts_write(evo_apts_writer *w, int fill, int n)
{
    if (!w || n <= 0)
        return;
    if (fill == 0)
        w->block_us = head_us(w);
    w->since += n;
}

void evo_apts_skip(evo_apts_writer *w, int n)
{
    if (w && n > 0)
        w->since += n;
}

int64_t evo_apts_block(const evo_apts_writer *w)
{
    return w ? w->block_us : EVO_AV_NOPTS;
}

/* ------------------------------------------------------------- output side */

void evo_aclock_reset(evo_aclock *c, int rate, int block_samples)
{
    if (!c)
        return;
    c->rate = rate > 0 ? rate : 48000;
    c->block_samples = block_samples > 0 ? block_samples : 2048;
    c->started = 0;
    c->base_us = EVO_AV_NOPTS;
    c->since = 0;
    c->own_anchor = EVO_AV_NOPTS;
    c->stale = 0;
}

int64_t evo_aclock_pts(const evo_aclock *c)
{
    if (!c || !c->started)
        return EVO_AV_NOPTS;
    return c->base_us + samples_us(c->since, c->rate);
}

int evo_aclock_stale(evo_aclock *c, int64_t pts_us, int64_t anchor_us)
{
    int64_t gap;
    if (!c || c->started || c->stale >= EVO_AV_STALE_MAX)
        return 0;
    if (pts_us == EVO_AV_NOPTS || anchor_us == EVO_AV_NOPTS)
        return 0;
    gap = pts_us > anchor_us ? pts_us - anchor_us : anchor_us - pts_us;
    if (gap <= EVO_AV_JUMP_US)
        return 0;
    c->stale++;
    return 1;
}

double evo_aclock_play(evo_aclock *c, int64_t pts_us, int64_t anchor_us, int64_t *shift_us)
{
    int64_t shift = 0;
    int64_t anchor;

    if (shift_us)
        *shift_us = 0;
    if (!c)
        return 0.0;

    if (pts_us != EVO_AV_NOPTS) {
        if (c->started) {
            /* Where this block would start if nothing had been lost. */
            const int64_t expected = c->base_us + samples_us(c->since + c->block_samples, c->rate);
            if (pts_us < expected - EVO_AV_JUMP_US)
                shift = pts_us - expected;
        }
        c->base_us = pts_us;
        c->since = 0;
    } else if (c->started) {
        c->since += c->block_samples;
    } else {
        /* Nothing to go on yet: count from the anchor. */
        c->base_us = anchor_us != EVO_AV_NOPTS ? anchor_us : 0;
        c->since = 0;
    }

    if (!c->started) {
        c->started = 1;
        c->own_anchor = c->base_us;
    }
    c->own_anchor += shift;

    anchor = anchor_us != EVO_AV_NOPTS ? anchor_us + shift : c->own_anchor;
    if (shift_us)
        *shift_us = shift;
    return (double)(evo_aclock_pts(c) - anchor) / 1000000.0;
}

/* ------------------------------------------------------------- the log line */

void evo_avsync_log_reset(evo_avsync_log *s)
{
    if (!s)
        return;
    s->last_ms = -1;
    s->logged_ms = 0.0;
}

int evo_avsync_log_due(evo_avsync_log *s, long long now_ms, double off_ms)
{
    int due;
    if (!s)
        return 0;
    if (s->last_ms < 0) {
        due = 1;
    } else {
        const long long since = now_ms - s->last_ms;
        const double moved = off_ms > s->logged_ms ? off_ms - s->logged_ms : s->logged_ms - off_ms;
        due = since >= EVO_AVSYNC_EVERY_MS || (moved > EVO_AVSYNC_STEP_MS && since >= EVO_AVSYNC_GAP_MS);
    }
    if (due) {
        s->last_ms = now_ms;
        s->logged_ms = off_ms;
    }
    return due;
}
