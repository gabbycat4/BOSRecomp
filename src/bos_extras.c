/*
 * bos_extras.c - Blades of Steel's additions to the NESRecomp cycle host
 * (cyc_host_extras.h). Presentation and observation only: nothing here
 * changes what the emulated machine does. Adapted from DD2Recomp's
 * dd2_extras.c.
 *
 *   Logs          --scroll-log FILE: one line per frame (scroll X/Y the PPU
 *                 drew the playfield with, its steps, the playfield rows,
 *                 sprites on screen, the busiest line's sprite count, the
 *                 smoother's shifts). --oam-log FILE: the frame's 64 OAM
 *                 entries. Both are for measuring the game.
 *
 *   Edge crop     config.ini [Game] OverscanTop/Bottom/Left/Right (default 8
 *                 each), --overscan T,B,L,R for one run.
 *
 *   No flicker    [Game] NoSpriteLimit (default 1): the NES draws at most 8
 *                 sprites per line, and the game takes turns when more share
 *                 a line (the flashing players). This redraws the picture
 *                 from the background and every sprite, every frame.
 *                 [Game] SpriteOrder: which sprite is on top where two
 *                 overlap. 0 the game's OAM order (changes every frame if
 *                 the game rotates its list), 1 (default) lower on screen in
 *                 front (players nearer the camera cover the ones behind).
 *
 *   Smooth camera [Game] SmoothCamera (default 1): evens out the camera's
 *                 steps by shifting the rink by up to SmoothMargin px (3);
 *                 the crop grows by that margin so no undrawn edge shows.
 *                 Sprites stay where the game drew them.
 *
 *   High refresh  [Game] PresentRate (0 = 60, or 120/180/240): extra
 *                 pictures between the NES's frames in which the camera
 *                 moves on a fraction of a pixel. The picture is always
 *                 drawn at PresentScale (4) times the NES size.
 */
#include "cyc_host_extras.h"
#include "cyc_core.h"
#include "cyc_render.h"
#include "bos_smooth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

enum {
    NES_W = 256,
    NES_H = 240,
    CROP_MAX = 64,
    SCROLL_W = 512,          /* playfield X spans two nametables, 0-511 */
    SCROLL_H = 480,          /* playfield Y, 0-479 */
    MIN_PLAYFIELD = 64,      /* rows of one continuous scroll to count as the rink */
    SCALE_MAX = 6,
};

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ---- settings ---- */
typedef struct { int top, bottom, left, right; } Crop;
#define BOS_DEFAULT_CROP { 8, 8, 8, 8 }
static Crop s_crop_saved = BOS_DEFAULT_CROP, s_crop = BOS_DEFAULT_CROP;
static int  s_smooth_saved = 1, s_smooth = 1;
static int  s_unlimit_saved = 1, s_unlimit = 1;
static int  s_order_saved = 1, s_order = 1;
static int  s_rate_saved, s_present_rate;
static int  s_scale_saved = 4, s_scale = 4;
static int  s_margin_saved = BOS_SMOOTH_MARGIN, s_margin = BOS_SMOOTH_MARGIN;

/* ---- per-frame state ---- */
static long          s_frame;
static CycHostExtras s_extras;              /* defined at the end */
static int           s_line = 120;          /* playfield line sampled */
static BosSmooth     s_smx, s_smy;
static int           s_prev_x = -1, s_prev_y = -1;
static int           s_top = -1, s_bot = -1; /* playfield rows [s_top, s_bot) */
static double        s_phase;               /* fraction of a frame since the machine frame */

static uint32_t s_bg[NES_W * NES_H];
static uint8_t  s_layer_at[NES_W * NES_H];  /* sprite pixel: 1 in front of the background, 2 behind */
static uint32_t s_layer[NES_W * NES_H];

/* Line y continues the playfield drawn on line y - 1: same X, next Y. */
static bool continues(int y)
{
    if (!(cyc_render_line_mask(y) & 0x08) || !(cyc_render_line_mask(y - 1) & 0x08)) return false;
    if (cyc_render_line_scroll_x(y) != cyc_render_line_scroll_x(y - 1)) return false;
    return cyc_render_line_scroll_y(y) == (cyc_render_line_scroll_y(y - 1) + 1) % SCROLL_H;
}

/* The scrolling part of the screen around s_line (the rink), or none. */
static void find_playfield(void)
{
    s_top = s_bot = -1;
    if (!(cyc_render_line_mask(s_line) & 0x08)) return;
    int t = s_line, b = s_line + 1;
    while (t > 0 && continues(t)) --t;
    while (b < NES_H && continues(b)) ++b;
    if (b - t < MIN_PLAYFIELD) return;
    s_top = t;
    s_bot = b;
}

/* ---- sprites ---- */
static int s_slots[64], s_nslots;          /* visible sprites, top priority first */

static void sprite_order(void)
{
    const uint8_t *oam = cyc_render_oam();
    s_nslots = 0;
    for (int slot = 0; slot < 64; ++slot)
        if (oam[slot * 4] < 0xEF) s_slots[s_nslots++] = slot;
    if (s_order != 1) return;
    /* Lower on screen in front; the same row keeps OAM order. */
    for (int i = 1; i < s_nslots; ++i) {
        int v = s_slots[i], j = i;
        while (j > 0 && oam[s_slots[j - 1] * 4] < oam[v * 4]) { s_slots[j] = s_slots[j - 1]; --j; }
        s_slots[j] = v;
    }
}

static uint16_t sprite_pattern(const uint8_t *o, bool tall, uint16_t table, int row)
{
    return tall ? (uint16_t)(((o[1] & 1) ? 0x1000 : 0) + (o[1] & 0xFE) * 16 + (row >= 8 ? 16 : 0))
                : (uint16_t)(table + o[1] * 16);
}

/* Every sprite on rows [y0, y1), no 8-per-line limit, in s_slots order:
 * s_layer gets the top opaque sprite pixel's colour, s_layer_at 1 (in front
 * of the background) or 2 (behind it). */
static void sprite_layer_all(int y0, int y1)
{
    memset(s_layer_at + y0 * NES_W, 0, (size_t)(y1 - y0) * NES_W);
    const uint8_t *oam = cyc_render_oam();
    for (int py = y0; py < y1; ++py) {
        uint8_t mask = cyc_render_line_mask(py);
        if (!(mask & 0x10)) continue;
        bool tall = cyc_render_line_sprite16(py);
        int rows = tall ? 16 : 8;
        uint16_t table = cyc_render_line_sprite_table(py);
        uint8_t *at = s_layer_at + py * NES_W;
        uint32_t *col = s_layer + py * NES_W;
        for (int k = 0; k < s_nslots; ++k) {
            const uint8_t *o = oam + s_slots[k] * 4;
            int r = py - (o[0] + 1);
            if (r < 0 || r >= rows) continue;
            unsigned attr = o[2];
            int row = (attr & 0x80) ? rows - 1 - r : r;
            uint16_t pattern = sprite_pattern(o, tall, table, row);
            uint8_t lo = cyc_render_chr((uint16_t)(pattern + (row & 7)));
            uint8_t hi = cyc_render_chr((uint16_t)(pattern + (row & 7) + 8));
            if (!(lo | hi)) continue;
            for (int c = 0; c < 8; ++c) {
                int px = o[3] + c;
                if (px >= NES_W) break;
                if (at[px] || (px < 8 && !(mask & 0x04))) continue;
                int bit = (attr & 0x40) ? c : 7 - c;
                int v = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);
                if (!v) continue;
                at[px] = (attr & 0x20) ? 2 : 1;
                col[px] = cyc_render_color(16 + (int)(attr & 3) * 4 + v);
            }
        }
    }
}

/* The sprites the NES itself drew (first 8 per line in OAM order), taken
 * from the native picture: used when NoSpriteLimit is off. Coverage comes
 * from OAM, never from comparing colours. */
static void sprite_layer_native(const uint32_t *native, int y0, int y1)
{
    memset(s_layer_at + y0 * NES_W, 0, (size_t)(y1 - y0) * NES_W);
    const uint8_t *oam = cyc_render_oam();
    const uint8_t *bg_opaque = cyc_frame_bg_opaque();
    for (int py = y0; py < y1; ++py) {
        uint8_t mask = cyc_render_line_mask(py);
        if (!(mask & 0x10)) continue;
        bool tall = cyc_render_line_sprite16(py);
        int rows = tall ? 16 : 8, found = 0;
        uint16_t table = cyc_render_line_sprite_table(py);
        uint8_t *at = s_layer_at + py * NES_W;
        uint32_t *col = s_layer + py * NES_W;
        const uint8_t *opaque = bg_opaque + py * NES_W;
        for (int slot = 0; slot < 64 && found < 8; ++slot) {
            const uint8_t *o = oam + slot * 4;
            int r = py - (o[0] + 1);
            if (o[0] >= 0xEF || r < 0 || r >= rows) continue;
            ++found;
            unsigned attr = o[2];
            int row = (attr & 0x80) ? rows - 1 - r : r;
            uint16_t pattern = sprite_pattern(o, tall, table, row);
            uint8_t lo = cyc_render_chr((uint16_t)(pattern + (row & 7)));
            uint8_t hi = cyc_render_chr((uint16_t)(pattern + (row & 7) + 8));
            for (int c = 0; c < 8; ++c) {
                int px = o[3] + c;
                if (px >= NES_W) break;
                if (at[px] || (px < 8 && !(mask & 0x04))) continue;
                int bit = (attr & 0x40) ? c : 7 - c;
                if (!(((lo >> bit) & 1) | (((hi >> bit) & 1) << 1))) continue;
                if ((attr & 0x20) && opaque[px]) { at[px] = 3; continue; }   /* hidden: taken, not shown */
                at[px] = 1;
                col[px] = native[py * NES_W + px];
            }
        }
        for (int x = 0; x < NES_W; ++x) if (at[x] == 3) at[x] = 0;
    }
}

/* Sprites on the busiest line (for the log: above 8 means the game flickers). */
static int busiest_line(void)
{
    const uint8_t *oam = cyc_render_oam();
    int most = 0;
    for (int py = 0; py < NES_H; ++py) {
        int rows = cyc_render_line_sprite16(py) ? 16 : 8, n = 0;
        for (int slot = 0; slot < 64; ++slot) {
            int r = py - (oam[slot * 4] + 1);
            if (oam[slot * 4] < 0xEF && r >= 0 && r < rows) ++n;
        }
        if (n > most) most = n;
    }
    return most;
}

/* ---- picture ---- */
static long s_prepared = -1;
static bool s_have_layers;

static void prepare(const uint32_t *native)
{
    s_prepared = s_frame;
    s_have_layers = cyc_render_background(s_bg);
    if (!s_have_layers) return;
    if (s_unlimit) { sprite_order(); sprite_layer_all(0, NES_H); }
    else sprite_layer_native(native, 0, NES_H);
}

static int floordiv(int a, int k) { return a >= 0 ? a / k : -((-a + k - 1) / k); }

static uint32_t s_big[NES_W * SCALE_MAX * NES_H * SCALE_MAX];
static int s_col_native[NES_W * SCALE_MAX], s_col_bg[NES_W * SCALE_MAX];

static const uint32_t *present(void *ctx, int *width, int *height)
{
    (void)ctx;
    int w, h;
    const uint32_t *native = cyc_render_present(&w, &h);
    if (!native || w != NES_W || h != NES_H) return NULL;
    if (s_prepared != s_frame) prepare(native);

    int k = s_scale;
    Crop c = s_crop;
    if (s_smooth) { c.left += s_margin; c.right += s_margin; c.top += s_margin; c.bottom += s_margin; }
    bool pf = s_smooth && s_top >= 0;
    double fx = 0, fy = 0;
    if (pf) {
        fx = s_smx.off - s_smx.vel * s_phase;
        fy = s_smy.off - s_smy.vel * s_phase;
        fx = fx < -s_margin ? -s_margin : fx > s_margin ? s_margin : fx;
        fy = fy < -s_margin ? -s_margin : fy > s_margin ? s_margin : fy;
    }
    int kx = (int)lround(fx * k), ky = (int)lround(fy * k);
    bool layers = s_have_layers && (kx || ky || s_unlimit);
    if (!layers && k == 1 && !c.top && !c.bottom && !c.left && !c.right) return NULL;

    int ow = (NES_W - c.left - c.right) * k, oh = (NES_H - c.top - c.bottom) * k;
    if (ow <= 0 || oh <= 0) return NULL;
    const uint8_t *opaque = cyc_frame_bg_opaque();
    for (int x = 0; x < ow; ++x) {
        int gx = c.left * k + x;
        s_col_native[x] = gx / k;
        s_col_bg[x] = clampi(floordiv(gx - kx, k), 0, NES_W - 1);
    }
    for (int y = 0; y < oh; ++y) {
        int gy = c.top * k + y, yn = gy / k;
        uint32_t *out = s_big + (size_t)y * (size_t)ow;
        if (!layers) {
            const uint32_t *row = native + yn * NES_W;
            for (int x = 0; x < ow; ++x) out[x] = row[s_col_native[x]];
            continue;
        }
        bool in_pf = pf && yn >= s_top && yn < s_bot;
        int by = in_pf ? clampi(floordiv(gy - ky, k), s_top, s_bot - 1) : yn;
        const uint32_t *bg = s_bg + by * NES_W;
        const uint8_t *op = opaque + by * NES_W;
        const uint8_t *at = s_layer_at + yn * NES_W;
        const uint32_t *sc = s_layer + yn * NES_W;
        for (int x = 0; x < ow; ++x) {
            int bx = in_pf ? s_col_bg[x] : s_col_native[x];
            int sx = s_col_native[x];
            uint32_t px = bg[bx];
            if (at[sx] == 1 || (at[sx] == 2 && !op[bx])) px = sc[sx];
            out[x] = px;
        }
    }
    *width = ow;
    *height = oh;
    return s_big;
}

static void present_phase(void *ctx, double phase)
{
    (void)ctx;
    s_phase = phase;
}

/* ---- settings ---- */
static bool parse_crop(const char *s, Crop *out)
{
    int t, b, l, r;
    char tail;
    if (!s || sscanf(s, "%d,%d,%d,%d%c", &t, &b, &l, &r, &tail) != 4) return false;
    if (t < 0 || b < 0 || l < 0 || r < 0 || t > CROP_MAX || b > CROP_MAX || l > CROP_MAX || r > CROP_MAX)
        return false;
    out->top = t; out->bottom = b; out->left = l; out->right = r;
    return true;
}

static void load_setting(void *ctx, const char *key, const char *value)
{
    (void)ctx;
    int v = atoi(value);
    if      (!strcmp(key, "OverscanTop"))    s_crop_saved.top = clampi(v, 0, CROP_MAX);
    else if (!strcmp(key, "OverscanBottom")) s_crop_saved.bottom = clampi(v, 0, CROP_MAX);
    else if (!strcmp(key, "OverscanLeft"))   s_crop_saved.left = clampi(v, 0, CROP_MAX);
    else if (!strcmp(key, "OverscanRight"))  s_crop_saved.right = clampi(v, 0, CROP_MAX);
    else if (!strcmp(key, "SmoothCamera"))   s_smooth = s_smooth_saved = v != 0;
    else if (!strcmp(key, "SmoothMargin"))   s_margin = s_margin_saved = clampi(v, 1, BOS_SMOOTH_MARGIN_MAX);
    else if (!strcmp(key, "NoSpriteLimit"))  s_unlimit = s_unlimit_saved = v != 0;
    else if (!strcmp(key, "SpriteOrder"))    s_order = s_order_saved = clampi(v, 0, 1);
    else if (!strcmp(key, "PresentRate"))    s_present_rate = s_rate_saved = clampi(v, 0, 1000);
    else if (!strcmp(key, "PresentScale"))   s_scale = s_scale_saved = clampi(v, 1, SCALE_MAX);
    s_crop = s_crop_saved;
}

static void save_settings(void *ctx, FILE *f)
{
    (void)ctx;
    fprintf(f, "OverscanTop = %d\n", s_crop_saved.top);
    fprintf(f, "OverscanBottom = %d\n", s_crop_saved.bottom);
    fprintf(f, "OverscanLeft = %d\n", s_crop_saved.left);
    fprintf(f, "OverscanRight = %d\n", s_crop_saved.right);
    fprintf(f, "SmoothCamera = %d\n", s_smooth_saved);
    fprintf(f, "SmoothMargin = %d\n", s_margin_saved);
    fprintf(f, "NoSpriteLimit = %d\n", s_unlimit_saved);
    fprintf(f, "SpriteOrder = %d\n", s_order_saved);
    fprintf(f, "PresentRate = %d\n", s_rate_saved);
    fprintf(f, "PresentScale = %d\n", s_scale_saved);
}

/* ---- per frame ---- */
static FILE *s_log, *s_oam_log;

static void restart(void)
{
    s_prev_x = s_prev_y = -1;
    bos_smooth_set_margin(&s_smx, s_margin, false);
    bos_smooth_set_margin(&s_smy, s_margin, false);
    bos_smooth_reset(&s_smx);
    bos_smooth_reset(&s_smy);
}

static void power_on(void *ctx)
{
    (void)ctx;
    s_frame = 0;
    restart();
    if (s_log) { fprintf(s_log, "# power on\n"); fflush(s_log); }
}

static void state_loaded(void *ctx)
{
    (void)ctx;
    restart();
    if (s_log) { fprintf(s_log, "# state loaded at frame %ld\n", s_frame); fflush(s_log); }
}

static int wrap_step(int d, int span) { d %= span; if (d < 0) d += span; return d >= span / 2 ? d - span : d; }

/* The camera's step: across two nametables (512) or, for a game that keeps
 * to one, 256; whichever is the shorter move. */
static int step_x(int d)
{
    int a = wrap_step(d, SCROLL_W), b = wrap_step(d, 256);
    return abs(b) < abs(a) ? b : a;
}

static void frame_end(void *ctx)
{
    (void)ctx;
    long f = s_frame++;
    static bool capture_on;
    s_phase = 0;
    s_extras.present_rate = (unsigned)s_present_rate;
    if (!capture_on) {
        cyc_render_capture_background(true);   /* whole from the next frame */
        capture_on = true;
    }

    find_playfield();
    bool on = s_top >= 0;
    int x = cyc_render_line_scroll_x(s_line);
    int y = cyc_render_line_scroll_y(s_line) - (s_line - s_top);   /* the playfield's top row */
    if (on) y = ((y % SCROLL_H) + SCROLL_H) % SCROLL_H;
    int dx = on && s_prev_x >= 0 ? step_x(x - s_prev_x) : 0;
    int dy = on && s_prev_y >= 0 ? wrap_step(y - s_prev_y, SCROLL_H) : 0;
    s_prev_x = on ? x : -1;
    s_prev_y = on ? y : -1;
    int ox = bos_smooth_frame(&s_smx, on, dx);
    int oy = bos_smooth_frame(&s_smy, on, dy);

    if (s_oam_log) {
        const uint8_t *oam = cyc_render_oam();
        fprintf(s_oam_log, "%ld", f);
        for (int i = 0; i < 64; ++i)
            fprintf(s_oam_log, " %02x%02x%02x%02x", oam[i * 4], oam[i * 4 + 1], oam[i * 4 + 2], oam[i * 4 + 3]);
        fputc('\n', s_oam_log);
    }
    if (!s_log) return;
    const uint8_t *oam = cyc_render_oam();
    int n = 0;
    for (int i = 0; i < 64; ++i) n += oam[i * 4] < 0xEF;
    if (!on) { fprintf(s_log, "%6ld  off  %3d  %3d\n", f, n, busiest_line()); fflush(s_log); return; }
    fprintf(s_log, "%6ld  %4d  %+3d  %4d  %+3d  %3d  %3d  %3d  %3d  %+2d  %+2d\n",
            f, x, dx, y, dy, s_top, s_bot, n, busiest_line(), ox, oy);
    fflush(s_log);
}

/* ---- options ---- */
static const CycHostOption s_options[] = {
    { "--scroll-log",      true, "FILE: log the playfield scroll and sprite counts every frame" },
    { "--oam-log",         true, "FILE: the frame's 64 sprite entries (Y tile attr X, hex) every frame" },
    { "--scroll-line",     true, "N: playfield line the scroll is sampled on (default 120)" },
    { "--overscan",        true, "T,B,L,R: crop this many pixels for this run" },
    { "--smooth",          true, "0|1: camera smoothing for this run" },
    { "--smooth-margin",   true, "N: smoothing margin in px, 1-12 (default 3)" },
    { "--no-sprite-limit", true, "0|1: draw every sprite (no flicker) for this run" },
    { "--sprite-order",    true, "0|1: overlaps in the game's order / lower on screen in front" },
    { "--present-rate",    true, "N: pictures per second, e.g. 120 or 240 (0: one per frame)" },
    { "--present-scale",   true, "K: picture scale 1-6 (default 4)" },
};

static bool parse_int(const char *value, int lo, int hi, int *out)
{
    char *end;
    long v = strtol(value, &end, 10);
    if (!*value || *end || v < lo || v > hi) return false;
    *out = (int)v;
    return true;
}

static bool option(void *ctx, const char *name, const char *value)
{
    (void)ctx;
    if (!strcmp(name, "--scroll-log") || !strcmp(name, "--oam-log")) {
        bool scroll = name[2] == 's';
        FILE **fp = scroll ? &s_log : &s_oam_log;
        if (*fp) fclose(*fp);
        *fp = fopen(value, "w");
        if (!*fp) { perror(value); return false; }
        if (scroll)
            fprintf(*fp, "# frame  scrollX  dx  scrollY  dy  top  bottom  sprites  busiestLine  shiftX  shiftY\n");
        return true;
    }
    if (!strcmp(name, "--scroll-line"))    return parse_int(value, 1, NES_H - 2, &s_line);
    if (!strcmp(name, "--overscan"))       return parse_crop(value, &s_crop);
    if (!strcmp(name, "--smooth"))         return parse_int(value, 0, 1, &s_smooth);
    if (!strcmp(name, "--smooth-margin"))  return parse_int(value, 1, BOS_SMOOTH_MARGIN_MAX, &s_margin);
    if (!strcmp(name, "--no-sprite-limit")) return parse_int(value, 0, 1, &s_unlimit);
    if (!strcmp(name, "--sprite-order"))   return parse_int(value, 0, 1, &s_order);
    if (!strcmp(name, "--present-scale"))  return parse_int(value, 1, SCALE_MAX, &s_scale);
    if (!strcmp(name, "--present-rate")) {
        if (!parse_int(value, 0, 1000, &s_present_rate)) return false;
        s_extras.present_rate = (unsigned)s_present_rate;
        return true;
    }
    return false;
}

/* ---- the in-game menu (recomp-ui runtime menu) ----
 * A change applies at once and is saved to config.ini [Game]. */
#ifdef CYC_WITH_RECOMP_UI
#include "recomp_runtime_ui.h"

static const char *const RATE_CHOICES[] = { "60 (off)", "120", "180", "240" };
static const int RATE_VALUES[] = { 0, 120, 180, 240 };
static const char *const ORDER_CHOICES[] = { "Game's order", "Lower on screen in front" };
static const char *const CROP_CHOICES[] = { "Off", "Standard", "More", "Custom (config.ini)" };
static const Crop CROP_PRESETS[] = { { 0, 0, 0, 0 }, { 8, 8, 8, 8 }, { 12, 12, 12, 12 } };

static const RecompRuntimeUiItem s_menu[] = {
    { "bos.smooth", "Smooth motion", "Smooth camera",
      "Evens out the rink's scrolling steps (sprites stay where the game drew them).",
      RECOMP_RUNTIME_UI_BOOL, 0, 1, 1, NULL, 0, NULL },
    { "bos.rate", "Smooth motion", "Pictures per second",
      "Extra pictures between the NES's 60 frames: the camera moves a fraction of a pixel in each. "
      "Pick your monitor's refresh rate or lower (180 suits a 240 Hz FreeSync monitor).",
      RECOMP_RUNTIME_UI_CHOICE, 0, 3, 1, RATE_CHOICES, 4, RATE_VALUES },
    { "bos.unlimit", "Picture", "No sprite flicker",
      "Draws every sprite every frame instead of the NES's 8 per line, so players stop flashing.",
      RECOMP_RUNTIME_UI_BOOL, 0, 1, 1, NULL, 0, NULL },
    { "bos.order", "Picture", "Overlapping sprites",
      "Which sprite is in front where two overlap (with No sprite flicker on).",
      RECOMP_RUNTIME_UI_CHOICE, 0, 1, 1, ORDER_CHOICES, 2, NULL },
    { "bos.crop", "Picture", "Edge crop",
      "Hides the screen edges where the NES draws scrolling garbage. Standard: 8 pixels.",
      RECOMP_RUNTIME_UI_CHOICE, 0, 3, 1, CROP_CHOICES, 4, NULL },
};

static bool key_is(const RecompRuntimeUiItem *it, const char *k) { return it->key && !strcmp(it->key, k); }

static int crop_preset(void)
{
    for (int i = 0; i < 3; ++i) {
        const Crop *c = &CROP_PRESETS[i];
        if (s_crop.top == c->top && s_crop.bottom == c->bottom && s_crop.left == c->left && s_crop.right == c->right)
            return i;
    }
    return 3;
}

static int menu_get(void *ctx, const RecompRuntimeUiItem *it, int *out)
{
    (void)ctx;
    if (key_is(it, "bos.smooth")) *out = s_smooth;
    else if (key_is(it, "bos.rate")) {
        *out = 0;
        for (int i = 0; i < 4; ++i) if (RATE_VALUES[i] == s_present_rate) *out = RATE_VALUES[i];
    }
    else if (key_is(it, "bos.unlimit")) *out = s_unlimit;
    else if (key_is(it, "bos.order")) *out = s_order;
    else if (key_is(it, "bos.crop")) *out = crop_preset();
    else return 0;
    return 1;
}

static int menu_set(void *ctx, const RecompRuntimeUiItem *it, int v)
{
    (void)ctx;
    if (key_is(it, "bos.smooth")) { s_smooth = s_smooth_saved = v != 0; restart(); }
    else if (key_is(it, "bos.rate")) {
        s_present_rate = s_rate_saved = clampi(v, 0, 1000);
        s_extras.present_rate = (unsigned)s_present_rate;
    }
    else if (key_is(it, "bos.unlimit")) { s_unlimit = s_unlimit_saved = v != 0; s_prepared = -1; }
    else if (key_is(it, "bos.order")) { s_order = s_order_saved = clampi(v, 0, 1); s_prepared = -1; }
    else if (key_is(it, "bos.crop")) {
        if (v < 0 || v > 2) return 0;              /* Custom: edit config.ini */
        s_crop = s_crop_saved = CROP_PRESETS[v];
    }
    else return 0;
    return 1;
}

static int menu_enabled(void *ctx, const RecompRuntimeUiItem *it)
{
    (void)ctx;
    if (key_is(it, "bos.order")) return s_unlimit;
    return 1;
}

static const RecompRuntimeUiCallbacks s_menu_callbacks = {
    .get_value = menu_get, .set_value = menu_set, .is_enabled = menu_enabled,
};
#define BOS_MENU_ITEMS     s_menu
#define BOS_MENU_COUNT     (sizeof(s_menu) / sizeof(s_menu[0]))
#define BOS_MENU_CALLBACKS (&s_menu_callbacks)
#else
#define BOS_MENU_ITEMS     NULL
#define BOS_MENU_COUNT     0
#define BOS_MENU_CALLBACKS NULL
#endif

static CycHostExtras s_extras = {
    .present         = present,
    .load_setting    = load_setting,
    .save_settings   = save_settings,
    .power_on        = power_on,
    .frame_end       = frame_end,
    .options         = s_options,
    .option_count    = sizeof(s_options) / sizeof(s_options[0]),
    .option          = option,
    .state_loaded    = state_loaded,
    .present_phase   = present_phase,
    .menu_items      = BOS_MENU_ITEMS,
    .menu_item_count = BOS_MENU_COUNT,
    .menu_callbacks  = BOS_MENU_CALLBACKS,
};

const CycHostExtras *cyc_host_extras(void) { return &s_extras; }
