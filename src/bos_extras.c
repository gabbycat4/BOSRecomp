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
 *   Flash soften  [Game] FlashSoften: the game flashes the player with the
 *                 puck by switching him to a bright palette 2 frames in 6.
 *                 0 as the game, 1 a soft tint, 2 no flash, 3 (default) a
 *                 slow pulse (fades in and out 1.25 times a second).
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

/* ---- flash softening (FlashSoften) ----
 * Measured (oam.log): the game flashes a player by switching his sprites
 * from his team's palette (0 or 1) to palette 3 for 2 frames out of every 6.
 * Each frame this matches every palette-3 sprite to the same tile in the
 * previous frame (allowing for the camera's step and a few px of movement);
 * if that one had another palette, or was itself a flash, the sprite is
 * flashing and s_base[slot] holds his own palette. Drawing then mixes the
 * flash colour into his own (1: soft) or leaves it out (2: none). Sprites
 * that are always palette 3 never match a different palette, so they stay. */
enum { FLASH_REACH = 8, FLASH_MAX_RUN = 3, FLASH_SOFT_PERCENT = 30,
       FLASH_HOLD = 8,           /* frames a player counts as flashing after his last flash */
       PULSE_PERIOD = 48,        /* frames per slow pulse (1.25 a second) */
       PULSE_PERCENT = 70 };     /* strongest mix of the flash colour in a pulse */
enum { SOFT_OFF, SOFT_SOFT, SOFT_NONE, SOFT_PULSE, SOFT_MODES };
typedef struct { uint8_t y, tile, attr, x; int8_t base; uint8_t run, age; } FlSprite;
static FlSprite s_fl_prev[64];
static int      s_fl_prev_n, s_flashing;
static int8_t   s_base[64];      /* flashing now: the player's own palette, else -1 */
static int8_t   s_own[64];       /* the sprite's own palette (base, or its palette) */
static uint8_t  s_age[64];       /* frames since the sprite last flashed, 255: not flashing */
static int      s_soft_saved = SOFT_PULSE, s_soft = SOFT_PULSE;

static bool touching(const uint8_t *a, const uint8_t *b, int h)
{
    return a[3] <= b[3] + 9 && b[3] <= a[3] + 9 && a[0] <= b[0] + h + 1 && b[0] <= a[0] + h + 1;
}

static void flash_frame(bool on, int dx, int dy)
{
    const uint8_t *oam = cyc_render_oam();
    FlSprite cur[64];
    int n = 0;
    s_flashing = 0;
    if (!on) dx = dy = 0;
    for (int slot = 0; slot < 64; ++slot) {
        const uint8_t *o = oam + slot * 4;
        s_base[slot] = -1;
        s_own[slot] = (int8_t)(o[2] & 3);
        s_age[slot] = 255;
        if (o[0] >= 0xEF) continue;
        FlSprite c = { o[0], o[1], o[2], o[3], -1, 0, 255 };
        int best = -1, bd = 1 << 30;
        for (int j = 0; j < s_fl_prev_n; ++j) {
            const FlSprite *p = &s_fl_prev[j];
            if (p->tile != o[1] || ((p->attr ^ o[2]) & 0xC0)) continue;
            int ex = o[3] - (p->x - dx), ey = o[0] - (p->y - dy);
            if (abs(ex) > FLASH_REACH || abs(ey) > FLASH_REACH) continue;
            int d = abs(ex) + abs(ey);
            if (d < bd) { bd = d; best = j; }
        }
        const FlSprite *p = best >= 0 ? &s_fl_prev[best] : NULL;
        if ((o[2] & 3) == 3) {
            /* Palette 3: a flash if the same sprite had a team palette (or was flashing). */
            if (p && (p->attr & 3) != 3) { c.base = (int8_t)(p->attr & 3); c.run = 1; }
            else if (p && p->base >= 0 && p->run < FLASH_MAX_RUN) { c.base = p->base; c.run = (uint8_t)(p->run + 1); }
            if (c.base >= 0) c.age = 0;
        } else if (p && p->age < FLASH_HOLD && (p->base == (int8_t)(o[2] & 3) || (p->attr & 3) == (o[2] & 3))) {
            c.age = (uint8_t)(p->age + 1);            /* between flashes: still the flashing player */
        }
        s_base[slot] = c.base;
        if (c.base >= 0) s_own[slot] = c.base;
        s_age[slot] = c.age;
        cur[n++] = c;
    }
    memcpy(s_fl_prev, cur, sizeof(cur[0]) * (size_t)n);
    s_fl_prev_n = n;

    /* A sprite that changed tile this frame (animation) has no match: give
     * it the flashing state of a touching sprite of the same player, so a
     * flash or pulse covers the whole player. */
    int h = cyc_render_line_sprite16(120) ? 16 : 8;
    for (int pass = 0; pass < 4; ++pass) {
        bool grew = false;
        for (int a = 0; a < 64; ++a) {
            const uint8_t *oa = oam + a * 4;
            if (oa[0] >= 0xEF || s_age[a] <= FLASH_HOLD) continue;
            for (int b = 0; b < 64; ++b) {
                const uint8_t *ob = oam + b * 4;
                if (b == a || ob[0] >= 0xEF || s_age[b] > FLASH_HOLD || !touching(oa, ob, h)) continue;
                bool pal3 = (oa[2] & 3) == 3;
                if (!pal3 && s_own[b] != (int8_t)(oa[2] & 3)) continue;
                if (pal3 && s_base[b] < 0) continue;  /* only a flashing neighbour vouches for palette 3 */
                s_age[a] = s_age[b];
                if (pal3) { s_base[a] = s_own[a] = s_own[b]; }
                grew = true;
                break;
            }
        }
        if (!grew) break;
    }
    for (int i = 0; i < 64; ++i) s_flashing += s_age[i] <= FLASH_HOLD;
}

static uint32_t mix(uint32_t a, uint32_t b, int pb)   /* pb percent of b */
{
    uint32_t out = a & 0xFF000000u;
    for (int sh = 0; sh < 24; sh += 8) {
        int ca = (int)((a >> sh) & 255), cb = (int)((b >> sh) & 255);
        out |= (uint32_t)((ca * (100 - pb) + cb * pb) / 100) << sh;
    }
    return out;
}

static long s_frame;
static int pulse_percent(void)
{
    double t = (double)(s_frame % PULSE_PERIOD) / PULSE_PERIOD;
    return (int)lround(PULSE_PERCENT * (0.5 - 0.5 * cos(2.0 * 3.14159265358979 * t)));
}

static bool flash_touched(int slot)
{
    if (!s_soft) return false;
    if (s_soft == SOFT_PULSE) return s_age[slot] <= FLASH_HOLD;
    return s_base[slot] >= 0;
}

static uint32_t sprite_colour(int slot, unsigned attr, int v)
{
    uint32_t c = cyc_render_color(16 + (int)(attr & 3) * 4 + v);
    if (!flash_touched(slot)) return c;
    uint32_t own = cyc_render_color(16 + s_own[slot] * 4 + v);
    uint32_t flash = cyc_render_color(16 + 3 * 4 + v);
    if (s_soft == SOFT_NONE) return own;
    if (s_soft == SOFT_SOFT) return mix(own, flash, FLASH_SOFT_PERCENT);
    return mix(own, flash, pulse_percent());
}

/* ---- sprites ---- */
static int s_slots[64], s_nslots;          /* visible sprites, top priority first */

/* Lower on screen in front. The game rotates its sprite list every frame,
 * so ties (same row) are broken by X, tile and attributes, never by slot:
 * the same overlap draws the same way every frame. */
static bool in_front(const uint8_t *a, const uint8_t *b)
{
    if (a[0] != b[0]) return a[0] > b[0];
    if (a[3] != b[3]) return a[3] < b[3];
    if (a[1] != b[1]) return a[1] < b[1];
    return a[2] < b[2];
}

static void sprite_order(void)
{
    const uint8_t *oam = cyc_render_oam();
    s_nslots = 0;
    for (int slot = 0; slot < 64; ++slot)
        if (oam[slot * 4] < 0xEF) s_slots[s_nslots++] = slot;
    if (s_order != 1) return;
    for (int i = 1; i < s_nslots; ++i) {
        int v = s_slots[i], j = i;
        while (j > 0 && in_front(oam + v * 4, oam + s_slots[j - 1] * 4)) { s_slots[j] = s_slots[j - 1]; --j; }
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
                col[px] = sprite_colour(s_slots[k], attr, v);
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
                int v = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);
                if (!v) continue;
                if ((attr & 0x20) && opaque[px]) { at[px] = 3; continue; }   /* hidden: taken, not shown */
                at[px] = 1;
                col[px] = flash_touched(slot) ? sprite_colour(slot, attr, v) : native[py * NES_W + px];
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
    else if (!strcmp(key, "FlashSoften"))    s_soft = s_soft_saved = clampi(v, 0, SOFT_MODES - 1);
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
    fprintf(f, "FlashSoften = %d\n", s_soft_saved);
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
    flash_frame(on, dx, dy);

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
    fprintf(s_log, "%6ld  %4d  %+3d  %4d  %+3d  %3d  %3d  %3d  %3d  %+2d  %+2d  %2d\n",
            f, x, dx, y, dy, s_top, s_bot, n, busiest_line(), ox, oy, s_flashing);
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
    { "--flash-soften",    true, "0|1|2|3: player flash as the game / soft / none / slow pulse" },
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
            fprintf(*fp, "# frame  scrollX  dx  scrollY  dy  top  bottom  sprites  busiestLine  shiftX  shiftY  flashing\n");
        return true;
    }
    if (!strcmp(name, "--scroll-line"))    return parse_int(value, 1, NES_H - 2, &s_line);
    if (!strcmp(name, "--overscan"))       return parse_crop(value, &s_crop);
    if (!strcmp(name, "--smooth"))         return parse_int(value, 0, 1, &s_smooth);
    if (!strcmp(name, "--smooth-margin"))  return parse_int(value, 1, BOS_SMOOTH_MARGIN_MAX, &s_margin);
    if (!strcmp(name, "--no-sprite-limit")) return parse_int(value, 0, 1, &s_unlimit);
    if (!strcmp(name, "--sprite-order"))   return parse_int(value, 0, 1, &s_order);
    if (!strcmp(name, "--flash-soften"))   return parse_int(value, 0, SOFT_MODES - 1, &s_soft);
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
static const char *const SOFT_CHOICES[] = { "Off (as the game)", "Soft", "Slow pulse", "None" };
static const int SOFT_VALUES[] = { SOFT_OFF, SOFT_SOFT, SOFT_PULSE, SOFT_NONE };
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
    { "bos.soften", "Picture", "Player flashing",
      "The game flashes the player with the puck bright white 10 times a second. Slow pulse: he fades "
      "gently in and out instead. Soft: a faint fast flash. None: no flash at all.",
      RECOMP_RUNTIME_UI_CHOICE, 0, 3, 1, SOFT_CHOICES, 4, SOFT_VALUES },
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
    else if (key_is(it, "bos.soften")) *out = s_soft;
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
    else if (key_is(it, "bos.soften")) { s_soft = s_soft_saved = clampi(v, 0, SOFT_MODES - 1); s_prepared = -1; }
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
