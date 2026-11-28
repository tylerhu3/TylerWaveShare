/* Panda: "Bao", a breathing buddy drawn entirely with LVGL shapes.
 *
 * Idle: Bao bobs, blinks, looks around, waves and chats in a speech bubble.
 * Boop his face for a giggle and a burst of hearts. Leave him alone for a
 * while and he dozes off (zzz) until touched.
 * Tap "Breathe with me" for a one-minute breathing session paced like the
 * original panda.gif (3.5 s in, 8.6 s hold, 6.7 s out, 0.4 s rest): the sun orb
 * swells and shrinks, the ring traces each phase and Bao breathes along.
 * Finishing sessions raises his Zen level and unlocks accessories (leaf,
 * flower, scarf, halo), saved in KV.
 *
 * All state lives in one PSRAM block (port_big_alloc) so .bss stays tiny.
 * No opacity on parents: that forces an LVGL layer buffer. Fades use
 * bg/text/image opa on leaf objects instead. */
#include "app.h"
#include "assets/assets.h"
#include "port.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TICK_MS   40
#define SCR       466
#define CX        233
#define CY        188 /* orb / panda centre, above the home handle */

#define PW        220 /* panda container */
#define PH        260
#define OY        14  /* headroom inside the container for the halo */
#define PLIFT     6   /* panda sits a little high in the orb */

#define ORB_MIN   236
#define ORB_SPAN  64
#define RING_D    324
#define DOT_Y     418 /* progress row under the pill / phase text */

/* Measured from panda.gif (192 frames x 100 ms = 19.2 s per breath) */
#define PHASE_IN_MS   3500
#define PHASE_HOLD_MS 8600
#define PHASE_OUT_MS  6700
#define PHASE_REST_MS 400
#define INTRO_MS      2400
#define DONE_MS       4800
#define BREATHS       3   /* 3 x 19.2 s + intro = one calm minute */
#define DOZE_MS       30000
#define PAUSE_GIVEUP  25000

#define N_FX   14
#define N_ZZZ  3
#define HEART_W 28
#define HEART_H 25
#define SPARK_D 18

#define KV_KEY   "panda_zen"
#define KV_MAGIC 0x50414E31u

#define COL_BG_TOP 0xB3A8DA
#define COL_BG_BOT 0x8D80C2
#define COL_ORB    0xFBE3A0
#define COL_GLOW   0xFFF3CF
#define COL_GOLD   0xF6C744
#define COL_INK    0x231E2E
#define COL_WHITE  0xFFFFFF
#define COL_PINK   0xFF9DB5
#define COL_TEXT   0x3B3060

typedef struct {
    uint32_t magic;
    uint32_t sessions;
    uint32_t breaths;
    uint32_t boops;
} zen_save_t;

typedef enum { ST_IDLE, ST_DOZE, ST_INTRO, ST_BREATHE, ST_PAUSED, ST_DONE } state_t;
typedef enum { EYE_OPEN, EYE_CLOSED, EYE_HAPPY } eye_t;
typedef enum { PH_IN, PH_HOLD, PH_OUT, PH_REST } phase_t;

typedef struct { uint16_t hz; uint8_t ticks; } note_t;

typedef struct {
    lv_obj_t *img;
    bool      on;
    float     x, y, vx, vy, g;
    int       life, max;
} fx_t;

typedef struct {
    /* objects */
    lv_obj_t *root, *glow, *orb, *ring, *pd;
    lv_obj_t *body, *arm_l, *arm_r, *eye[2], *pupil[2], *lid[2];
    lv_obj_t *mouth[2], *mouth_open, *blush[2];
    lv_obj_t *gift[4];
    lv_obj_t *bubble, *bubble_lbl, *tail[2];
    lv_obj_t *pill, *pill_lbl, *phase_lbl, *info_lbl, *dot[BREATHS];
    lv_obj_t *zzz[N_ZZZ];
    lv_timer_t *timer;

    fx_t     fx[N_FX];
    int      zzz_life[N_ZZZ];

    /* A8 sprites, recoloured per particle */
    uint8_t        heart_px[HEART_W * HEART_H];
    uint8_t        spark_px[SPARK_D * SPARK_D];
    lv_image_dsc_t heart, spark;

    zen_save_t save;
    state_t    st;
    eye_t      eye_mode;
    phase_t    phase;
    int        breath;      /* 0..BREATHS-1 */
    uint32_t   now;         /* app clock, ms */
    uint32_t   st_t0;       /* when the current state started */
    uint32_t   ph_t0;       /* when the current phase started */
    uint32_t   pause_t0;
    uint32_t   last_touch;
    uint32_t   last_tick;
    float      b;           /* breath fullness 0..1 */

    uint32_t   blink_at, blink_until;
    uint32_t   look_at;
    int        look_dx, look_cur;
    uint32_t   wave_t0;
    bool       waving;
    uint32_t   hop_t0;
    bool       hopping;
    uint32_t   happy_until;
    uint32_t   bubble_at, bubble_until, bubble_t0;
    int        bubble_idx;
    uint32_t   zzz_at, spark_at;
    int        level;
    int        new_gift;    /* level whose gift just unlocked, 0 = none */

    const char *phase_txt;
    const char *pill_txt;
    char        msg[40];

    lv_point_t press;
    bool       pressed;

    const note_t *mel;
    int        mel_idx, mel_rem;
    bool       tone_on;
} panda_t;

static panda_t *P;

/* ── sounds ─────────────────────────────────────────────── */
static const note_t MEL_BOOP[]  = { {1047, 1}, {1319, 2}, {0, 0} };
static const note_t MEL_IN[]    = { {523, 2}, {659, 3}, {0, 0} };
static const note_t MEL_OUT[]   = { {587, 2}, {440, 3}, {0, 0} };
static const note_t MEL_DONE[]  = { {523, 2}, {659, 2}, {784, 2}, {1047, 5}, {0, 0} };
static const note_t MEL_GIFT[]  = { {784, 2}, {988, 2}, {1175, 2}, {1568, 6}, {0, 0} };
static const note_t MEL_WAKE[]  = { {880, 1}, {0, 1}, {1175, 2}, {0, 0} };

static void tone_stop(void)
{
    P->mel = NULL;
    if (P->tone_on) {
        port_tone_enable(false);
        port_tone_hold(false);
        P->tone_on = false;
    }
}

static void tone_play(const note_t *m)
{
    P->mel = m;
    P->mel_idx = 0;
    P->mel_rem = 0;
    if (!P->tone_on) {
        port_tone_hold(true);
        P->tone_on = true;
    }
}

static void tone_step(void)
{
    if (!P->mel) return;
    if (P->mel_rem > 0 && --P->mel_rem > 0) return;
    const note_t *n = &P->mel[P->mel_idx];
    if (n->ticks == 0) { tone_stop(); return; }
    if (n->hz) { port_tone_freq(n->hz); port_tone_enable(true); }
    else         port_tone_enable(false);
    P->mel_rem = n->ticks;
    P->mel_idx++;
}

/* ── words ──────────────────────────────────────────────── */
static const char *const INVITES[] = {
    "Breathe with me?", "One calm minute?", "Boop my nose!",
    "You look great today", "Let's relax together", "I saved you a bamboo",
};
static const char *const BOOPS[] = {
    "Hehe!", "That tickles!", "Boop!", "*giggle*", "Again! Again!", "You're sweet",
};
static const char *const PRAISE[] = {
    "So calm!", "Lovely breathing", "I feel cozy now", "Thank you, friend",
};
static const char *const GIFT_NAME[] = {
    "", "", "a bamboo leaf!", "a little flower!", "a cozy scarf!", "a golden halo!",
};
#define NOF(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* ── helpers ────────────────────────────────────────────── */
static lv_obj_t *blob(lv_obj_t *p, int x, int y, int w, int h, uint32_t col)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(col), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}

static lv_obj_t *group(lv_obj_t *p, int x, int y, int w, int h)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    return o;
}

static lv_obj_t *curve(lv_obj_t *p, int cx, int cy, int d, int w, uint32_t col)
{
    lv_obj_t *a = lv_arc_create(p);
    lv_obj_remove_style(a, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(a, d, d);
    lv_obj_set_pos(a, cx - d / 2, cy - d / 2);
    lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, w, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(a, lv_color_hex(col), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_INDICATOR);
    lv_arc_set_rotation(a, 0);
    lv_arc_set_bg_angles(a, 0, 360);
    return a;
}

static void curve_angles(lv_obj_t *a, int start, int end)
{
    lv_arc_set_angles(a, (lv_value_precise_t)start, (lv_value_precise_t)end);
}

static void show(lv_obj_t *o, bool on)
{
    if (!o) return;
    if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static float frand(float lo, float hi) { return lo + (hi - lo) * (float)(rand() % 1000) / 1000.0f; }

static void set_text(lv_obj_t *l, const char **cache, const char *t)
{
    if (*cache == t) return;
    *cache = t;
    lv_label_set_text(l, t);
}

static void level_info(uint32_t s, int *lvl, int *into, int *span)
{
    /* Level k+1 starts at the k-th triangular number of sessions. */
    int k = 0;
    while ((uint32_t)((k + 1) * (k + 2) / 2) <= s) k++;
    *lvl = k + 1;
    *into = (int)s - k * (k + 1) / 2;
    *span = k + 1;
}

/* ── sprites ────────────────────────────────────────────── */
static void bake_sprites(void)
{
    for (int y = 0; y < HEART_H; y++)
        for (int x = 0; x < HEART_W; x++) {
            int hit = 0;
            for (int sy = 0; sy < 3; sy++)
                for (int sx = 0; sx < 3; sx++) {
                    float u = ((x + (sx + 0.5f) / 3.0f) / HEART_W - 0.5f) * 2.5f;
                    float v = (0.42f - (y + (sy + 0.5f) / 3.0f) / HEART_H) * 2.6f;
                    float q = u * u + v * v - 1.0f;
                    if (q * q * q - u * u * v * v * v <= 0.0f) hit++;
                }
            P->heart_px[y * HEART_W + x] = (uint8_t)(hit * 255 / 9);
        }
    for (int y = 0; y < SPARK_D; y++)
        for (int x = 0; x < SPARK_D; x++) {
            int hit = 0;
            for (int sy = 0; sy < 3; sy++)
                for (int sx = 0; sx < 3; sx++) {
                    float u = fabsf(((x + (sx + 0.5f) / 3.0f) / SPARK_D - 0.5f) * 2.0f);
                    float v = fabsf(((y + (sy + 0.5f) / 3.0f) / SPARK_D - 0.5f) * 2.0f);
                    if (sqrtf(u) + sqrtf(v) <= 1.0f) hit++;
                }
            P->spark_px[y * SPARK_D + x] = (uint8_t)(hit * 255 / 9);
        }

    P->heart.header.magic  = LV_IMAGE_HEADER_MAGIC;
    P->heart.header.cf     = LV_COLOR_FORMAT_A8;
    P->heart.header.w      = HEART_W;
    P->heart.header.h      = HEART_H;
    P->heart.header.stride = HEART_W;
    P->heart.data_size     = sizeof(P->heart_px);
    P->heart.data          = P->heart_px;

    P->spark.header.magic  = LV_IMAGE_HEADER_MAGIC;
    P->spark.header.cf     = LV_COLOR_FORMAT_A8;
    P->spark.header.w      = SPARK_D;
    P->spark.header.h      = SPARK_D;
    P->spark.header.stride = SPARK_D;
    P->spark.data_size     = sizeof(P->spark_px);
    P->spark.data          = P->spark_px;
}

static void fx_spawn(bool heart, float x, float y, float vx, float vy, float g,
                     int life, uint32_t col)
{
    for (int i = 0; i < N_FX; i++) {
        fx_t *f = &P->fx[i];
        if (f->on) continue;
        f->on = true;
        f->x = x; f->y = y; f->vx = vx; f->vy = vy; f->g = g;
        f->life = f->max = life;
        lv_image_set_src(f->img, heart ? (const void *)&P->heart : (const void *)&P->spark);
        lv_obj_set_style_image_recolor(f->img, lv_color_hex(col), 0);
        lv_obj_set_style_image_recolor_opa(f->img, LV_OPA_COVER, 0);
        lv_obj_set_pos(f->img, (int)x, (int)y);
        show(f->img, true);
        return;
    }
}

static void burst_hearts(int n, float x, float y)
{
    static const uint32_t cols[] = { 0xFF6F96, 0xFF9DB5, 0xF6587E, COL_GOLD };
    for (int i = 0; i < n; i++) {
        bool h = (i % 3) != 2;
        fx_spawn(h, x - HEART_W / 2 + frand(-50, 50), y + frand(-10, 10),
                 frand(-3.0f, 3.0f), frand(-6.0f, -3.0f), 0.2f,
                 22 + rand() % 10, cols[rand() % 4]);
    }
}

static void fx_step(void)
{
    for (int i = 0; i < N_FX; i++) {
        fx_t *f = &P->fx[i];
        if (!f->on) continue;
        if (--f->life <= 0) { f->on = false; show(f->img, false); continue; }
        f->vy += f->g;
        f->x += f->vx;
        f->y += f->vy;
        lv_obj_set_pos(f->img, (int)f->x, (int)f->y);
        int opa = f->life * 3 * 255 / f->max;
        lv_obj_set_style_image_opa(f->img, (lv_opa_t)(opa > 255 ? 255 : opa), 0);
    }
}

/* ── panda drawing ──────────────────────────────────────── */
static void build_panda(lv_obj_t *p)
{
    lv_obj_t *pd = group(p, CX - PW / 2, CY - PH / 2 - PLIFT, PW, PH);
    P->pd = pd;

    blob(pd, 28, OY + 6, 58, 58, COL_INK);   /* ears */
    blob(pd, 134, OY + 6, 58, 58, COL_INK);
    blob(pd, 42, OY + 20, 20, 20, 0x4A4258); /* inner ear glints */
    blob(pd, 158, OY + 20, 20, 20, 0x4A4258);

    P->body = blob(pd, 44, OY + 132, 132, 108, COL_WHITE);
    lv_obj_set_style_border_width(P->body, 3, 0);
    lv_obj_set_style_border_color(P->body, lv_color_hex(COL_INK), 0);
    blob(pd, 78, OY + 168, 64, 52, 0xF4EEF7); /* soft belly */

    for (int i = 0; i < 2; i++) {
        int fx = i ? 114 : 56;
        blob(pd, fx, OY + 212, 50, 36, COL_INK);
        blob(pd, fx + 15, OY + 220, 20, 16, COL_PINK);
    }

    lv_obj_t *head = blob(pd, 26, OY + 20, 168, 138, COL_WHITE);
    lv_obj_set_style_border_width(head, 3, 0);
    lv_obj_set_style_border_color(head, lv_color_hex(COL_INK), 0);

    for (int i = 0; i < 2; i++) {
        int pcx = i ? 143 : 77, pcy = OY + 91;
        blob(pd, pcx - 23, pcy - 27, 46, 54, COL_INK);
        P->blush[i] = blob(pd, i ? 156 : 38, OY + 118, 28, 14, COL_PINK);
        lv_obj_set_style_bg_opa(P->blush[i], 170, 0);

        /* open eye: white with a pupil and a sparkle */
        P->eye[i] = blob(pd, pcx - 10 + (i ? -2 : 2), pcy - 12, 20, 22, COL_WHITE);
        P->pupil[i] = blob(P->eye[i], 4, 6, 12, 13, COL_INK);
        blob(P->pupil[i], 2, 2, 4, 4, COL_WHITE);

        /* closed / happy eye */
        P->lid[i] = curve(pd, pcx, pcy - 2, 26, 4, COL_WHITE);
        show(P->lid[i], false);
    }

    blob(pd, 100, OY + 108, 20, 13, COL_INK); /* nose */
    blob(pd, 104, OY + 110, 6, 3, 0x6A6278);  /* nose shine */
    P->mouth[0] = curve(pd, 104, OY + 122, 14, 3, COL_INK);
    P->mouth[1] = curve(pd, 116, OY + 122, 14, 3, COL_INK);
    curve_angles(P->mouth[0], 10, 170);
    curve_angles(P->mouth[1], 10, 170);
    P->mouth_open = blob(pd, 101, OY + 124, 18, 16, 0xE8607A);
    lv_obj_set_style_border_width(P->mouth_open, 2, 0);
    lv_obj_set_style_border_color(P->mouth_open, lv_color_hex(COL_INK), 0);
    show(P->mouth_open, false);

    /* gift 3: scarf (drawn over the chin, under the arms) */
    lv_obj_t *scarf = group(pd, 56, OY + 146, 108, 40);
    lv_obj_t *band = blob(scarf, 0, 0, 108, 18, 0xE8576B);
    lv_obj_set_style_radius(band, 9, 0);
    lv_obj_t *tailk = blob(scarf, 70, 10, 18, 28, 0xD94A60);
    lv_obj_set_style_radius(tailk, 6, 0);
    P->gift[2] = scarf;

    P->arm_l = blob(pd, 22, OY + 140, 44, 62, COL_INK);
    P->arm_r = blob(pd, 154, OY + 140, 44, 62, COL_INK);

    /* gift 1: bamboo leaf sprouting from the head */
    lv_obj_t *leaf = group(pd, 96, OY - 4, 44, 30);
    blob(leaf, 12, 8, 5, 20, 0x3F8F4A);
    lv_obj_t *l1 = blob(leaf, 14, 0, 30, 13, 0x6CC070);
    lv_obj_set_style_radius(l1, 7, 0);
    lv_obj_t *l2 = blob(leaf, 0, 9, 16, 9, 0x5BB060);
    lv_obj_set_style_radius(l2, 5, 0);
    P->gift[0] = leaf;

    /* gift 2: flower by the right ear */
    lv_obj_t *fl = group(pd, 150, OY + 26, 36, 36);
    static const int8_t pet[5][2] = { {11, 0}, {22, 8}, {18, 21}, {4, 21}, {0, 8} };
    for (int i = 0; i < 5; i++) blob(fl, pet[i][0], pet[i][1], 15, 15, 0xFF8FB1);
    blob(fl, 11, 11, 13, 13, 0xFFD45E);
    P->gift[1] = fl;

    /* gift 4: halo */
    lv_obj_t *halo = blob(pd, 64, 0, 92, 20, COL_GOLD);
    lv_obj_set_style_bg_opa(halo, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(halo, 4, 0);
    lv_obj_set_style_border_color(halo, lv_color_hex(COL_GOLD), 0);
    P->gift[3] = halo;
}

static void eyes(eye_t m)
{
    if (P->eye_mode == m) return;
    P->eye_mode = m;
    for (int i = 0; i < 2; i++) {
        show(P->eye[i], m == EYE_OPEN);
        show(P->lid[i], m != EYE_OPEN);
        if (m == EYE_CLOSED) curve_angles(P->lid[i], 20, 160);  /* ∪ */
        if (m == EYE_HAPPY)  curve_angles(P->lid[i], 200, 340); /* ∩ */
    }
}

static void mouth_open(bool on)
{
    show(P->mouth_open, on);
    show(P->mouth[0], !on);
    show(P->mouth[1], !on);
}

static void apply_gifts(void)
{
    for (int i = 0; i < 4; i++) show(P->gift[i], P->level >= i + 2);
}

/* ── UI text ────────────────────────────────────────────── */
static void bubble_say(const char *t, uint32_t ms)
{
    lv_label_set_text(P->bubble_lbl, t);
    P->bubble_t0 = P->now;
    P->bubble_until = P->now + ms;
    show(P->bubble, true);
    show(P->tail[0], true);
    show(P->tail[1], true);
}

static void bubble_step(void)
{
    if (lv_obj_has_flag(P->bubble, LV_OBJ_FLAG_HIDDEN)) return;
    uint32_t age = P->now - P->bubble_t0;
    int opa = 255;
    int dy = 0;
    if (age < 200) { opa = (int)age * 255 / 200; dy = 8 - (int)age * 8 / 200; }
    if (P->now + 250 > P->bubble_until) {
        int left = (int)(P->bubble_until - P->now);
        if (left <= 0) {
            show(P->bubble, false); show(P->tail[0], false); show(P->tail[1], false);
            return;
        }
        opa = left * 255 / 250;
    }
    lv_obj_set_style_bg_opa(P->bubble, (lv_opa_t)(opa * 240 / 255), 0);
    lv_obj_set_style_text_opa(P->bubble_lbl, (lv_opa_t)opa, 0);
    for (int i = 0; i < 2; i++) lv_obj_set_style_bg_opa(P->tail[i], (lv_opa_t)(opa * 240 / 255), 0);
    lv_obj_align(P->bubble, LV_ALIGN_TOP_MID, 0, 26 + dy);
}

static void refresh_info(void)
{
    int lvl, into, span;
    level_info(P->save.sessions, &lvl, &into, &span);
    P->level = lvl;
    if (P->save.sessions == 0)
        lv_label_set_text(P->info_lbl, "1 calm minute with Bao");
    else
        lv_label_set_text_fmt(P->info_lbl, "Zen Lv %d   |   %d to grow", lvl, span - into);
    lv_arc_set_value(P->ring, into * 1000 / span);
}

static void ui_for_state(void)
{
    bool idle = P->st == ST_IDLE || P->st == ST_DOZE;
    show(P->pill, idle);
    show(P->info_lbl, idle);
    show(P->phase_lbl, !idle);
    for (int i = 0; i < BREATHS; i++) show(P->dot[i], P->st == ST_BREATHE || P->st == ST_PAUSED);
    if (idle) refresh_info();
}

static void dots_update(void)
{
    for (int i = 0; i < BREATHS; i++) {
        bool done = i < P->breath;
        bool cur = i == P->breath;
        lv_obj_set_style_bg_color(P->dot[i], lv_color_hex(done || cur ? COL_GOLD : COL_WHITE), 0);
        lv_obj_set_style_bg_opa(P->dot[i], done ? LV_OPA_COVER : (cur ? 200 : 110), 0);
    }
}

/* ── state changes ──────────────────────────────────────── */
static void go(state_t s)
{
    P->st = s;
    P->st_t0 = P->now;
    ui_for_state();
}

static void start_session(void)
{
    P->breath = 0;
    P->waving = false;
    mouth_open(false);
    eyes(EYE_CLOSED);
    set_text(P->phase_lbl, &P->phase_txt, "Get comfy...");
    bubble_say("Close your eyes with me", 2200);
    go(ST_INTRO);
    dots_update();
}

static void begin_phase(phase_t ph)
{
    P->phase = ph;
    P->ph_t0 = P->now;
    static const char *const T[] = { "Breathe in", "Hold", "Breathe out" };
    if (ph != PH_REST) set_text(P->phase_lbl, &P->phase_txt, T[ph]); /* rest keeps "Breathe out" */
    if (ph == PH_IN)  tone_play(MEL_IN);
    if (ph == PH_OUT) tone_play(MEL_OUT);
}

static void finish_session(void)
{
    int old = P->level;
    P->save.sessions++;
    P->save.breaths += BREATHS;
    port_kv_write(KV_KEY, &P->save, sizeof(P->save));
    int lvl, into, span;
    level_info(P->save.sessions, &lvl, &into, &span);
    P->new_gift = (lvl > old && lvl >= 2 && lvl <= 5) ? lvl : 0;
    P->level = lvl;
    apply_gifts();

    eyes(EYE_HAPPY);
    mouth_open(true);
    P->hopping = true;
    P->hop_t0 = P->now;
    P->breath = BREATHS;
    dots_update();
    burst_hearts(10, CX, CY - 100);
    if (P->new_gift) {
        lv_snprintf(P->msg, sizeof(P->msg), "New gift: %s", GIFT_NAME[P->new_gift]);
        bubble_say(P->msg, DONE_MS);
        set_text(P->phase_lbl, &P->phase_txt, "Zen level up!");
        tone_play(MEL_GIFT);
    } else {
        bubble_say(PRAISE[rand() % NOF(PRAISE)], 3000);
        set_text(P->phase_lbl, &P->phase_txt, "1 calm minute done");
        tone_play(MEL_DONE);
    }
    go(ST_DONE);
}

static void to_idle(void)
{
    eyes(EYE_OPEN);
    mouth_open(false);
    P->last_touch = P->now;
    P->bubble_at = P->now + 6000;
    go(ST_IDLE);
}

static void boop(void)
{
    P->save.boops++;
    eyes(EYE_HAPPY);
    mouth_open(true);
    P->happy_until = P->now + 900;
    P->hopping = true;
    P->hop_t0 = P->now;
    burst_hearts(5, CX, CY - 100);
    tone_play(MEL_BOOP);
    if (P->save.boops % 25 == 0) bubble_say("Best friends forever!", 2400);
    else                         bubble_say(BOOPS[rand() % NOF(BOOPS)], 1600);
    if (P->save.boops % 5 == 0) port_kv_write(KV_KEY, &P->save, sizeof(P->save));
}

static void wake(void)
{
    for (int i = 0; i < N_ZZZ; i++) { P->zzz_life[i] = 0; show(P->zzz[i], false); }
    eyes(EYE_HAPPY);
    P->happy_until = P->now + 700;
    P->hopping = true;
    P->hop_t0 = P->now;
    tone_play(MEL_WAKE);
    bubble_say("Oh! Hi there!", 1800);
    to_idle();
}

/* ── touch ──────────────────────────────────────────────── */
static void touch_cb(lv_event_t *e)
{
    if (!P) return;
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *in = lv_indev_active();
    lv_point_t p = { 0, 0 };
    if (in) lv_indev_get_point(in, &p);

    if (code == LV_EVENT_PRESSED) {
        P->press = p;
        P->pressed = !launcher_handle_zone(p.x, p.y);
        return;
    }
    if (code != LV_EVENT_RELEASED || !P->pressed) return;
    P->pressed = false;
    int dx = p.x - P->press.x, dy = p.y - P->press.y;
    if (dx * dx + dy * dy >= 60 * 60) return; /* a swipe, not a tap */
    P->last_touch = P->now;

    int px = p.x - CX, py = p.y - CY;
    bool on_panda = px * px + py * py < 112 * 112;
    bool on_pill  = abs(p.x - CX) < 118 && p.y > 350 && p.y < 404;

    switch (P->st) {
    case ST_DOZE:
        wake();
        break;
    case ST_IDLE:
        if (on_pill)       start_session();
        else if (on_panda) boop();
        break;
    case ST_INTRO:
    case ST_BREATHE:
        P->pause_t0 = P->now;
        eyes(EYE_OPEN);
        set_text(P->phase_lbl, &P->phase_txt, "Paused - tap to go on");
        go(ST_PAUSED);
        break;
    case ST_PAUSED:
        /* resume from the start of the interrupted breath */
        eyes(EYE_CLOSED);
        go(ST_BREATHE);
        begin_phase(PH_IN);
        break;
    case ST_DONE:
        to_idle();
        break;
    }
}

/* ── the tick ───────────────────────────────────────────── */
static void idle_life(void)
{
    uint32_t t = P->now;

    /* blink */
    if (P->eye_mode != EYE_HAPPY || t > P->happy_until) {
        if (t >= P->blink_at && t < P->blink_at + 140) eyes(EYE_CLOSED);
        else {
            eyes(EYE_OPEN);
            mouth_open(false);
            if (t >= P->blink_at + 140)
                P->blink_at = t + 2200 + (uint32_t)(rand() % 3200);
        }
    }

    /* look around */
    if (t >= P->look_at) {
        static const int8_t L[] = { -4, 0, 0, 4, 2, -2 };
        P->look_dx = L[rand() % 6];
        P->look_at = t + 1600 + (uint32_t)(rand() % 2600);
    }
    if (P->look_cur != P->look_dx) {
        P->look_cur += (P->look_dx > P->look_cur) ? 1 : -1;
        for (int i = 0; i < 2; i++) lv_obj_set_x(P->pupil[i], 4 + P->look_cur);
    }

    /* chat + wave */
    if (t >= P->bubble_at) {
        bubble_say(INVITES[P->bubble_idx], 3200);
        P->bubble_idx = (P->bubble_idx + 1) % NOF(INVITES);
        P->bubble_at = t + 8500;
        if (P->bubble_idx % 2 == 1) { P->waving = true; P->wave_t0 = t; }
    }

    if (t - P->last_touch > DOZE_MS) {
        eyes(EYE_CLOSED);
        P->zzz_at = t + 400;
        show(P->bubble, false); show(P->tail[0], false); show(P->tail[1], false);
        go(ST_DOZE);
    }
}

static void doze_life(void)
{
    if (P->now >= P->zzz_at) {
        P->zzz_at = P->now + 1300;
        for (int i = 0; i < N_ZZZ; i++) {
            if (P->zzz_life[i] > 0) continue;
            P->zzz_life[i] = 50;
            lv_obj_set_style_text_font(P->zzz[i], i == 1 ? &lv_font_montserrat_32 : &lv_font_montserrat_24, 0);
            break;
        }
    }
}

static void zzz_step(void)
{
    for (int i = 0; i < N_ZZZ; i++) {
        if (P->zzz_life[i] <= 0) continue;
        int age = 50 - --P->zzz_life[i];
        if (P->zzz_life[i] == 0) { show(P->zzz[i], false); continue; }
        show(P->zzz[i], true);
        int x = CX + 50 + age + (int)(6 * sinf(age * 0.25f));
        int y = CY - 70 - age * 2;
        lv_obj_set_pos(P->zzz[i], x, y);
        int opa = age < 8 ? age * 32 : P->zzz_life[i] * 10;
        lv_obj_set_style_text_opa(P->zzz[i], (lv_opa_t)(opa > 255 ? 255 : opa), 0);
    }
}

static void session_step(void)
{
    uint32_t t = P->now;
    if (P->st == ST_INTRO) {
        uint32_t a = t - P->st_t0;
        P->b = P->b * 0.92f; /* settle to empty */
        lv_arc_set_value(P->ring, (int32_t)(a * 1000 / INTRO_MS));
        if (a >= INTRO_MS) {
            go(ST_BREATHE);
            dots_update();
            begin_phase(PH_IN);
        }
        return;
    }
    if (P->st == ST_PAUSED) {
        if (t - P->pause_t0 > PAUSE_GIVEUP) {
            bubble_say("Let's try again later", 2400);
            to_idle();
        }
        return;
    }
    if (P->st != ST_BREATHE) return;

    static const uint32_t DUR[] = { PHASE_IN_MS, PHASE_HOLD_MS, PHASE_OUT_MS, PHASE_REST_MS };
    uint32_t a = t - P->ph_t0;
    float p = (float)a / (float)DUR[P->phase];
    if (p > 1.0f) p = 1.0f;
    if (P->phase == PH_IN)        P->b = 0.5f - 0.5f * cosf(3.14159f * p);
    else if (P->phase == PH_HOLD) P->b = 1.0f;
    else if (P->phase == PH_OUT)  P->b = 0.5f + 0.5f * cosf(3.14159f * p);
    else                          P->b = 0.0f;
    lv_arc_set_value(P->ring, P->phase == PH_REST ? 1000 : (int32_t)(p * 1000.0f));

    /* sparkles drift off the orb while breathing out */
    if (P->phase == PH_OUT && t >= P->spark_at) {
        P->spark_at = t + 380;
        float ang = frand(0, 6.283f);
        float r = (ORB_MIN + ORB_SPAN * P->b) * 0.5f;
        fx_spawn(false, CX + cosf(ang) * r - SPARK_D / 2, CY + sinf(ang) * r - SPARK_D / 2,
                 cosf(ang) * 1.6f, sinf(ang) * 1.6f, 0, 26, rand() % 2 ? COL_WHITE : COL_GOLD);
    }

    if (a < DUR[P->phase]) return;
    if (P->phase == PH_IN)        begin_phase(PH_HOLD);
    else if (P->phase == PH_HOLD) begin_phase(PH_OUT);
    else if (P->phase == PH_REST) begin_phase(PH_IN);
    else {
        P->breath++;
        dots_update();
        if (P->breath >= BREATHS) finish_session();
        else                      begin_phase(PH_REST);
    }
}

static void layout_step(void)
{
    uint32_t t = P->now;
    bool calm = P->st == ST_INTRO || P->st == ST_BREATHE || P->st == ST_PAUSED;

    if (!calm) P->b += ((0.35f + 0.08f * sinf(t * 0.0021f)) - P->b) * 0.08f;
    float b = P->b;

    int d = ORB_MIN + (int)(ORB_SPAN * b);
    lv_obj_set_size(P->orb, d, d);
    lv_obj_set_pos(P->orb, CX - d / 2, CY - d / 2);
    int g = d + 26;
    lv_obj_set_size(P->glow, g, g);
    lv_obj_set_pos(P->glow, CX - g / 2, CY - g / 2);
    lv_obj_set_style_bg_opa(P->glow, (lv_opa_t)(60 + 70 * b), 0);

    /* panda: bob when idle, rise with the breath when calm, hop on joy */
    int dy;
    if (calm)               dy = -(int)(10 * b);
    else if (P->st == ST_DOZE) dy = (int)(3 * sinf(t * 0.0018f)) + 4;
    else                    dy = (int)(3 * sinf(t * 0.0026f));
    if (P->hopping) {
        uint32_t a = t - P->hop_t0;
        uint32_t len = P->st == ST_DONE ? 1200 : 420;
        if (a >= len) P->hopping = false;
        else {
            float ph = (float)a / 420.0f;
            dy -= (int)(16 * fabsf(sinf(3.14159f * ph)));
        }
    }
    lv_obj_set_pos(P->pd, CX - PW / 2, CY - PH / 2 - PLIFT + dy);

    int grow = (int)(10 * b);
    lv_obj_set_x(P->body, 44 - grow / 2);
    lv_obj_set_width(P->body, 132 + grow);
    lv_obj_set_x(P->arm_l, 22 - grow / 2);

    /* right arm waves */
    int ax = 154 + grow / 2, ay = OY + 140;
    if (P->waving) {
        uint32_t a = t - P->wave_t0;
        if (a >= 1500) P->waving = false;
        else {
            float env = sinf(3.14159f * (float)a / 1500.0f);
            ax += (int)(18 * env + 6 * env * sinf(a * 0.02f));
            ay -= (int)(40 * env);
        }
    }
    lv_obj_set_pos(P->arm_r, ax, ay);

    /* halo floats */
    if (P->level >= 5) lv_obj_set_y(P->gift[3], (int)(2 * sinf(t * 0.004f)));

    /* the pill breathes gently to invite a tap */
    if (!lv_obj_has_flag(P->pill, LV_OBJ_FLAG_HIDDEN))
        lv_obj_set_style_border_opa(P->pill, (lv_opa_t)(150 + 100 * sinf(t * 0.004f)), 0);

    /* the current breath dot pulses */
    if ((P->st == ST_BREATHE) && P->breath < BREATHS) {
        int s = 12 + (int)(4 * b);
        for (int i = 0; i < BREATHS; i++) {
            int si = i == P->breath ? s : 12;
            lv_obj_set_size(P->dot[i], si, si);
            lv_obj_set_pos(P->dot[i], CX + (i - BREATHS / 2) * 26 - si / 2, DOT_Y - si / 2);
        }
    }
}

static void tick_cb(lv_timer_t *tm)
{
    (void)tm;
    if (!P) return;
    /* Never call lv_timer_set_period() here; just skip while the screen is off */
    if (launcher_screen_is_off()) {
        tone_stop();
        if (P->st == ST_BREATHE || P->st == ST_INTRO) {
            P->pause_t0 = P->now;
            eyes(EYE_OPEN);
            set_text(P->phase_lbl, &P->phase_txt, "Paused - tap to go on");
            go(ST_PAUSED);
        }
        P->last_tick = lv_tick_get();
        return;
    }
    uint32_t el = lv_tick_elaps(P->last_tick);
    P->last_tick = lv_tick_get();
    if (el > 120) el = 120;
    P->now += el;

    tone_step();
    switch (P->st) {
    case ST_IDLE: idle_life(); break;
    case ST_DOZE: doze_life(); break;
    case ST_DONE:
        if (P->now - P->st_t0 > DONE_MS) {
            if (P->new_gift) burst_hearts(6, CX, CY - 110);
            to_idle();
            P->pill_txt = NULL;
            set_text(P->pill_lbl, &P->pill_txt, "Breathe again");
        } else if (P->new_gift && (P->now - P->st_t0) % 600 < 40) {
            fx_spawn(false, CX + frand(-90, 90), CY - 120 + frand(-10, 30), 0, -0.8f, 0, 22, COL_GOLD);
        }
        break;
    default: session_step(); break;
    }
    layout_step();
    bubble_step();
    zzz_step();
    fx_step();
}

/* ── lifecycle ──────────────────────────────────────────── */
static void enter(lv_obj_t *root)
{
    port_crumb(CRUMB_GAME);
    P = (panda_t *)port_big_alloc(sizeof(panda_t));
    if (!P) {
        lv_obj_t *err = lv_label_create(root);
        lv_label_set_text(err, "Out of memory");
        lv_obj_set_style_text_color(err, lv_color_white(), 0);
        lv_obj_center(err);
        return;
    }
    memset(P, 0, sizeof(*P));
    srand(lv_tick_get());
    bake_sprites();

    zen_save_t sv;
    if (port_kv_read(KV_KEY, &sv, sizeof(sv)) && sv.magic == KV_MAGIC) P->save = sv;
    else P->save.magic = KV_MAGIC;

    lv_obj_t *bg = lv_obj_create(root);
    lv_obj_remove_style_all(bg);
    lv_obj_remove_flag(bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(bg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(bg, SCR, SCR);
    lv_obj_center(bg);
    lv_obj_set_style_bg_color(bg, lv_color_hex(COL_BG_TOP), 0);
    lv_obj_set_style_bg_grad_color(bg, lv_color_hex(COL_BG_BOT), 0);
    lv_obj_set_style_bg_grad_dir(bg, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(bg, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(bg, touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(bg, touch_cb, LV_EVENT_RELEASED, NULL);
    P->root = bg;

    /* a few tiny background stars */
    static const int16_t st[][2] = { {78, 120}, {392, 150}, {60, 300}, {404, 296}, {120, 60}, {350, 70} };
    for (int i = 0; i < 6; i++) {
        lv_obj_t *s = blob(bg, st[i][0], st[i][1], 5, 5, COL_WHITE);
        lv_obj_set_style_bg_opa(s, 120, 0);
    }

    P->glow = blob(bg, 0, 0, 10, 10, COL_GLOW);
    P->orb  = blob(bg, 0, 0, 10, 10, COL_ORB);

    P->ring = lv_arc_create(bg);
    lv_obj_remove_flag(P->ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(P->ring, RING_D, RING_D);
    lv_obj_set_pos(P->ring, CX - RING_D / 2, CY - RING_D / 2);
    lv_arc_set_rotation(P->ring, 270);
    lv_arc_set_bg_angles(P->ring, 0, 360);
    lv_arc_set_range(P->ring, 0, 1000);
    lv_obj_set_style_arc_width(P->ring, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_color(P->ring, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(P->ring, 110, LV_PART_MAIN);
    lv_obj_set_style_arc_width(P->ring, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(P->ring, lv_color_hex(COL_GOLD), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(P->ring, true, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(P->ring, lv_color_hex(COL_GOLD), LV_PART_KNOB);
    lv_obj_set_style_pad_all(P->ring, 5, LV_PART_KNOB);
    lv_obj_set_style_border_width(P->ring, 3, LV_PART_KNOB);
    lv_obj_set_style_border_color(P->ring, lv_color_hex(COL_GLOW), LV_PART_KNOB);
    lv_obj_set_style_shadow_width(P->ring, 0, LV_PART_KNOB);

    build_panda(bg);

    /* particles above the panda, under the bubble */
    for (int i = 0; i < N_FX; i++) {
        P->fx[i].img = lv_image_create(bg);
        lv_image_set_src(P->fx[i].img, &P->heart);
        show(P->fx[i].img, false);
    }

    /* speech bubble at the top, with thought-bubble tail dots */
    P->tail[0] = blob(bg, CX + 4, 70, 10, 10, COL_WHITE);
    P->tail[1] = blob(bg, CX + 14, 82, 6, 6, COL_WHITE);
    P->bubble = lv_obj_create(bg);
    lv_obj_remove_style_all(P->bubble);
    lv_obj_remove_flag(P->bubble, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(P->bubble, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(P->bubble, lv_color_white(), 0);
    lv_obj_set_style_radius(P->bubble, 20, 0);
    lv_obj_set_style_pad_hor(P->bubble, 16, 0);
    lv_obj_set_style_pad_ver(P->bubble, 10, 0);
    lv_obj_align(P->bubble, LV_ALIGN_TOP_MID, 0, 26);
    P->bubble_lbl = lv_label_create(P->bubble);
    lv_obj_set_style_text_font(P->bubble_lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(P->bubble_lbl, lv_color_hex(COL_TEXT), 0);
    lv_label_set_text(P->bubble_lbl, "");
    show(P->bubble, false); show(P->tail[0], false); show(P->tail[1], false);

    for (int i = 0; i < N_ZZZ; i++) {
        P->zzz[i] = lv_label_create(bg);
        lv_label_set_text(P->zzz[i], i == 1 ? "Z" : "z");
        lv_obj_set_style_text_color(P->zzz[i], lv_color_white(), 0);
        show(P->zzz[i], false);
    }

    /* bottom: invite pill / phase text */
    P->info_lbl = lv_label_create(bg);
    lv_obj_set_style_text_font(P->info_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(P->info_lbl, lv_color_white(), 0);
    lv_obj_align(P->info_lbl, LV_ALIGN_TOP_MID, 0, 408);

    for (int i = 0; i < BREATHS; i++) {
        P->dot[i] = blob(bg, CX + (i - BREATHS / 2) * 26 - 6, DOT_Y - 6, 12, 12, COL_WHITE);
    }

    P->pill = lv_obj_create(bg);
    lv_obj_remove_style_all(P->pill);
    lv_obj_remove_flag(P->pill, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(P->pill, 228, 48);
    lv_obj_align(P->pill, LV_ALIGN_TOP_MID, 0, 354);
    lv_obj_set_style_radius(P->pill, 24, 0);
    lv_obj_set_style_bg_color(P->pill, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(P->pill, 240, 0);
    lv_obj_set_style_border_width(P->pill, 3, 0);
    lv_obj_set_style_border_color(P->pill, lv_color_hex(COL_GOLD), 0);
    P->pill_lbl = lv_label_create(P->pill);
    lv_obj_set_style_text_font(P->pill_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(P->pill_lbl, lv_color_hex(COL_TEXT), 0);
    lv_obj_center(P->pill_lbl);
    set_text(P->pill_lbl, &P->pill_txt, "Breathe with me");

    P->phase_lbl = lv_label_create(bg);
    lv_obj_set_style_text_font(P->phase_lbl, &lv_font_montserrat_26, 0);
    lv_obj_set_style_text_color(P->phase_lbl, lv_color_white(), 0);
    lv_obj_align(P->phase_lbl, LV_ALIGN_TOP_MID, 0, 362);
    lv_obj_set_width(P->phase_lbl, 300);
    lv_obj_set_style_text_align(P->phase_lbl, LV_TEXT_ALIGN_CENTER, 0);

    P->eye_mode = EYE_CLOSED; /* force the first eyes() to apply */
    eyes(EYE_OPEN);
    P->b = 0.35f;
    P->last_tick = lv_tick_get();
    P->blink_at = 1800;
    P->look_at = 2500;
    P->bubble_at = 6500;
    refresh_info();
    apply_gifts();
    to_idle();

    /* greet: newcomers get an introduction, regulars a welcome back */
    P->waving = true;
    P->wave_t0 = 0;
    if (P->save.sessions == 0) bubble_say("Hi! I'm Bao", 2600);
    else if (P->save.sessions < 5) bubble_say("Welcome back!", 2400);
    else bubble_say("I missed you!", 2400);
    P->bubble_at = 5200;

    P->timer = lv_timer_create(tick_cb, TICK_MS, NULL);
}

static void leave(void)
{
    if (!P) return;
    if (P->timer) lv_timer_delete(P->timer);
    tone_stop();
    if (P->save.magic == KV_MAGIC) port_kv_write(KV_KEY, &P->save, sizeof(P->save));
    /* Objects first (the particle images point into P), then the memory */
    if (P->root && lv_obj_is_valid(P->root)) lv_obj_delete(P->root);
    port_big_free(P);
    P = NULL;
}

static lv_color_t tint(void) { return lv_color_hex(COL_GOLD); }

const badge_app_t app_panda = {
    .name       = "Panda",
    .art        = &app_icon_panda,
    .icon       = LV_SYMBOL_PLAY,
    .tint       = tint,
    .radio      = RADIO_OFF,
    .keep_awake = true,
    .enter      = enter,
    .leave      = leave,
};
