/* CheerLeader: a Tamagotchi-style pixel companion with FEED and TRAIN buttons,
 * tap-to-cheer comic-book pep talks, and accelerometer-triggered running. */
#include "app.h"
#include "port.h"
#include "assets/assets.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define SPR_W      18
#define SPR_H      14
#define PIX_SCALE  9
#define CAN_W      (SPR_W * PIX_SCALE)   /* 162 */
#define CAN_H      (SPR_H * PIX_SCALE)   /* 126 */
#define BG_HEX     0x0E1018

#define KV_KEY     "cheer_pet"
#define KV_MAGIC   0x43485231u

typedef struct {
    uint32_t magic;
    int32_t  hunger;     /* 0..100: <30 skinny, 30..70 normal, >70 chubby */
    int32_t  fitness;    /* 0..100: <30 weak,   30..70 normal, >70 buff   */
    uint32_t cheers;
    int64_t  saved_ts;   /* unix seconds when saved (0 if clock unset)    */
} cheer_save_t;

typedef enum {
    ACT_IDLE = 0,
    ACT_CHEER,
    ACT_EAT,
    ACT_TRAIN,
    ACT_RUN,
} act_t;

static const char *const CHEERS[] = {
    "YOU GOT THIS!",
    "GO! GO! GO!",
    "CRUSHING IT!",
    "SUPERSTAR!",
    "KEEP SHINING!",
    "UNSTOPPABLE!",
    "WAY TO GO!",
    "LEVEL UP!",
    "NICE WORK!",
    "NAILED IT!",
    "SO PROUD OF U!",
    "100% AWESOME!",
    "HIGH FIVE!",
    "PURE MAGIC!",
    "STAY AMAZING!",
    "CHAMPION!",
};
#define CHEER_N (sizeof(CHEERS) / sizeof(CHEERS[0]))

static const char *const EAT_LINES[] = {
    "YUM!!",
    "SO TASTY!!",
    "DELICIOUS!!",
    "YUMMY! NOM NOM!",
    "SCRUMPTIOUS!",
    "MUNCH MUNCH!",
    "BEST SNACK!",
    "SO GOOD!!",
};
#define EAT_N (sizeof(EAT_LINES) / sizeof(EAT_LINES[0]))
static const char *const FULL_LINES[] = {
    "SO FULL! *BURP*",
    "CHONKY MODE ON!",
    "TOO MUCH FOOD!",
};
static const char *const TRAIN_LINES[] = {
    "FEEL THE BURN!",
    "ONE MORE REP!",
    "GETTING BUFF!",
    "NO PAIN NO GAIN!",
};
static const char *const HUNGRY_LINES[] = {
    "SO HUNGRY...",
    "FEED ME PLS!",
    "TUMMY RUMBLING!",
};
static const char *const RUN_LINES[] = {
    "RUN! RUN! RUN!",
    "ZOOMIES!!",
    "FAST FEET!!",
    "KEEP MOVING!",
};

typedef struct {
    uint16_t hz;      /* 0 = rest */
    uint8_t  ticks;   /* duration in 50 ms ticks */
} note_t;

/* Cheer fanfares */
static const note_t MEL_CHEER_0[] = {
    { 523, 2 }, { 659, 2 }, { 784, 2 }, { 1047, 4 }, { 0, 0 }
};
static const note_t MEL_CHEER_1[] = {
    { 784, 2 }, { 0, 1 }, { 784, 2 }, { 880, 2 }, { 1047, 4 }, { 0, 0 }
};
static const note_t MEL_CHEER_2[] = {
    { 659, 1 }, { 784, 1 }, { 988, 2 }, { 1047, 2 }, { 1319, 4 }, { 0, 0 }
};
/* Cute munching & workout jingles */
static const note_t MEL_EAT[] = {
    { 440, 1 }, { 554, 1 }, { 659, 2 }, { 554, 1 }, { 659, 2 }, { 0, 0 }
};
static const note_t MEL_TRAIN[] = {
    { 392, 2 }, { 523, 2 }, { 659, 2 }, { 784, 3 }, { 0, 0 }
};
static const note_t MEL_UH_OH[] = {
    { 330, 2 }, { 262, 4 }, { 0, 0 }
};
static const note_t MEL_RUN[] = {
    { 587, 1 }, { 740, 1 }, { 880, 1 }, { 1175, 2 }, { 0, 0 }
};
static const note_t *const CHEER_MELS[] = { MEL_CHEER_0, MEL_CHEER_1, MEL_CHEER_2 };

static lv_obj_t   *s_canvas;
static uint16_t   *s_buf;
static lv_obj_t   *s_shadow_gnd;
static lv_obj_t   *s_stat_lbl;
static lv_obj_t   *s_state_lbl;
static lv_obj_t   *s_bubble;
static lv_obj_t   *s_bubble_sh;
static lv_obj_t   *s_tail_out;
static lv_obj_t   *s_tail_in;
static lv_obj_t   *s_cheer_lbl;
static lv_timer_t *s_timer;

static lv_point_t  s_press_pt;
static cheer_save_t s_pet;
static uint32_t    s_last_cheer;
static uint32_t    s_last_eat;
static uint32_t    s_tick;
static act_t       s_act;
static int         s_act_ticks;
static uint32_t    s_last_sig;

/* Accelerometer change detection for auto-running */
static bool        s_have_imu;
static float       s_prev_ax, s_prev_ay, s_prev_az;
static int         s_run_sound_cd;
static int         s_run_fit_acc;

/* Active chiptune jingle state */
static const note_t *s_melody;
static int           s_mel_idx;
static int           s_mel_rem;
static bool          s_tone_active;

static int clamp100(int v) { return v < 0 ? 0 : (v > 100 ? 100 : v); }

static uint16_t rgb565_hex(uint32_t hex)
{
    uint16_t r = (uint16_t)((hex >> 16) & 0xFF) >> 3;
    uint16_t g = (uint16_t)((hex >>  8) & 0xFF) >> 2;
    uint16_t b = (uint16_t)( hex        & 0xFF) >> 3;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static void pet_save(void)
{
    s_pet.magic = KV_MAGIC;
    time_t now = time(NULL);
    s_pet.saved_ts = (now > 1700000000) ? (int64_t)now : 0;
    port_kv_write(KV_KEY, &s_pet, sizeof(s_pet));
}

static void pet_load(void)
{
    cheer_save_t tmp;
    if (port_kv_read(KV_KEY, &tmp, sizeof(tmp)) && tmp.magic == KV_MAGIC) {
        s_pet = tmp;
        s_pet.hunger  = clamp100(s_pet.hunger);
        s_pet.fitness = clamp100(s_pet.fitness);
        /* Apply gentle real-time decay if wall clock is valid */
        time_t now = time(NULL);
        if (now > 1700000000 && s_pet.saved_ts > 1700000000 && now > s_pet.saved_ts) {
            int64_t mins = (now - s_pet.saved_ts) / 60;
            if (mins > 0) {
                s_pet.hunger  = clamp100(s_pet.hunger  - (int)(mins / 5));
                s_pet.fitness = clamp100(s_pet.fitness - (int)(mins / 10));
            }
        }
    } else {
        s_pet.magic = KV_MAGIC;
        s_pet.hunger = 50;
        s_pet.fitness = 50;
        s_pet.cheers = 0;
        s_pet.saved_ts = 0;
    }
}

static void tone_stop(void)
{
    s_melody = NULL;
    s_mel_idx = 0;
    s_mel_rem = 0;
    if (s_tone_active) {
        port_tone_enable(false);
        port_tone_hold(false);
        s_tone_active = false;
    }
}

static void tone_play(const note_t *mel)
{
    s_melody  = mel;
    s_mel_idx = 0;
    s_mel_rem = 0;
    if (!s_tone_active) {
        port_tone_hold(true);
        s_tone_active = true;
    }
}

static void tone_step(void)
{
    if (!s_melody) return;
    if (s_mel_rem > 0) {
        s_mel_rem--;
        if (s_mel_rem > 0) return;
        s_mel_idx++;
    }
    const note_t *n = &s_melody[s_mel_idx];
    if (n->ticks == 0) {
        tone_stop();
        return;
    }
    if (n->hz > 0) {
        port_tone_freq(n->hz);
        port_tone_enable(true);
    } else {
        port_tone_enable(false);
    }
    s_mel_rem = n->ticks;
}

/* ── Dynamic 18x14 pixel-art Tamagotchi renderer ────────────── */
static void put_px(uint16_t *grid, int x, int y, uint16_t col)
{
    if (x < 0 || x >= SPR_W || y < 0 || y >= SPR_H) return;
    grid[y * SPR_W + x] = col;
}

static void draw_pet(int phase)
{
    if (!s_buf || !s_canvas) return;

    int body_tier = (s_pet.hunger < 30) ? 0 : (s_pet.hunger > 70 ? 2 : 1);
    int str_tier  = (s_pet.fitness < 30) ? 0 : (s_pet.fitness > 70 ? 2 : 1);
    uint32_t sig = ((uint32_t)body_tier << 8) | ((uint32_t)str_tier << 6)
                 | ((uint32_t)s_act << 3) | (uint32_t)(phase & 7);
    if (sig == s_last_sig) return;
    s_last_sig = sig;

    const uint16_t BG   = rgb565_hex(BG_HEX);
    const uint16_t INK  = rgb565_hex(0x1A1826);
    const uint16_t BODY = rgb565_hex(body_tier == 0 ? 0xE6DC9C : 0xFFE680);
    const uint16_t SHAD = rgb565_hex(body_tier == 0 ? 0xC8B86A : 0xF5C242);
    const uint16_t PINK = rgb565_hex(0xFF6B9D);
    const uint16_t CYAN = rgb565_hex(0x5CE1E6);
    const uint16_t WHT  = rgb565_hex(0xFFFFFF);
    const uint16_t RED  = rgb565_hex(0xFF4D6D);
    const uint16_t GOLD = rgb565_hex(0xFFD23F);
    const uint16_t IRON = rgb565_hex(0x9BA4B5);

    uint16_t g[SPR_W * SPR_H];
    for (int i = 0; i < SPR_W * SPR_H; i++) g[i] = BG;

    /* Vertical bounce offset: jump on cheer, stride bob on run, squish on idle */
    int yoff = 0;
    if (s_act == ACT_CHEER) yoff = -2 + (phase & 1);
    else if (s_act == ACT_RUN) yoff = -(phase & 1);
    else if (s_act == ACT_IDLE && (phase & 1)) yoff = 1;

    /* Body half-width: skinny=3, normal=4, chubby=6 */
    int hw = (body_tier == 0) ? 3 : (body_tier == 2 ? 6 : 4);
    int top_y = 3 + yoff;
    int bot_y = 11 + yoff;
    int cx = 8;   /* center lies between col 8 and 9 */

    /* Cute little ears / head tufts */
    put_px(g, cx - 2, top_y - 1, INK);
    put_px(g, cx - 1, top_y - 1, INK);
    put_px(g, cx + 2, top_y - 1, INK);
    put_px(g, cx + 3, top_y - 1, INK);

    /* Draw torso with rounded corners + dark outline */
    for (int y = top_y; y <= bot_y; y++) {
        int row_hw = hw;
        if (y == top_y || y == bot_y) row_hw = hw - 1;
        else if (body_tier == 2 && y >= top_y + 3 && y <= bot_y - 1) row_hw = hw + 1;
        int x0 = cx - row_hw;
        int x1 = cx + 1 + row_hw;
        for (int x = x0; x <= x1; x++) {
            bool edge = (x == x0 || x == x1 || y == top_y || y == bot_y);
            uint16_t col = edge ? INK : (y >= bot_y - 2 ? SHAD : BODY);
            put_px(g, x, y, col);
        }
    }

    /* Chubby tummy shine */
    if (body_tier == 2) {
        put_px(g, cx,     bot_y - 2, WHT);
        put_px(g, cx + 1, bot_y - 2, WHT);
    }

    /* Buff headband when strong (fitness > 70) */
    if (str_tier == 2) {
        int hx0 = cx - (hw - 1);
        int hx1 = cx + 1 + (hw - 1);
        for (int x = hx0; x <= hx1; x++) put_px(g, x, top_y + 1, RED);
        put_px(g, cx,     top_y + 1, GOLD);
        put_px(g, cx + 1, top_y + 1, GOLD);
    }

    /* Feet (alternating stride + dust puffs when running) */
    int fy = bot_y + 1;
    if (s_act == ACT_RUN) {
        if (phase & 1) {
            put_px(g, cx - 3, fy, INK); put_px(g, cx - 2, fy, INK);
            put_px(g, cx + 3, fy - 1, INK); put_px(g, cx + 4, fy - 1, INK);
            put_px(g, 1, fy, WHT); put_px(g, 2, fy - 1, IRON);
        } else {
            put_px(g, cx - 1, fy - 1, INK); put_px(g, cx, fy - 1, INK);
            put_px(g, cx + 2, fy, INK); put_px(g, cx + 3, fy, INK);
            put_px(g, 2, fy, WHT); put_px(g, 0, fy - 1, IRON);
        }
    } else {
        put_px(g, cx - 2, fy, INK); put_px(g, cx - 1, fy, INK);
        put_px(g, cx + 2, fy, INK); put_px(g, cx + 3, fy, INK);
    }

    /* Eyes & cheeks */
    int ey = top_y + 3;
    bool happy_eyes = (s_act == ACT_CHEER || s_act == ACT_EAT || (s_act == ACT_IDLE && (phase & 1)));
    if (happy_eyes) {
        /* Cute ^ ^ / squinting happy eyes */
        put_px(g, cx - 2, ey, INK); put_px(g, cx - 1, ey, INK);
        put_px(g, cx + 2, ey, INK); put_px(g, cx + 3, ey, INK);
    } else if (body_tier == 0 && s_act == ACT_IDLE) {
        /* Hungry pleading eyes */
        put_px(g, cx - 2, ey, INK); put_px(g, cx - 2, ey + 1, WHT);
        put_px(g, cx + 3, ey, INK); put_px(g, cx + 3, ey + 1, WHT);
    } else {
        /* Big sparkly Tamagotchi eyes */
        put_px(g, cx - 2, ey, INK);     put_px(g, cx - 2, ey + 1, INK);
        put_px(g, cx - 1, ey, WHT);
        put_px(g, cx + 3, ey, INK);     put_px(g, cx + 3, ey + 1, INK);
        put_px(g, cx + 2, ey, WHT);
    }
    /* Rosy cheeks */
    put_px(g, cx - 3, ey + 1, PINK);
    put_px(g, cx + 4, ey + 1, PINK);

    /* Mouth */
    int my = ey + 2;
    if (s_act == ACT_CHEER) {
        put_px(g, cx, my, RED); put_px(g, cx + 1, my, RED);
        put_px(g, cx, my + 1, RED); put_px(g, cx + 1, my + 1, RED);
    } else if (s_act == ACT_EAT) {
        if (phase & 1) {
            put_px(g, cx, my, RED); put_px(g, cx + 1, my, RED);
            put_px(g, cx, my + 1, RED); put_px(g, cx + 1, my + 1, RED);
        } else {
            put_px(g, cx, my, INK); put_px(g, cx + 1, my, INK);
        }
    } else if (body_tier == 0 || str_tier == 0) {
        put_px(g, cx, my + 1, INK); put_px(g, cx + 1, my + 1, INK);
    } else {
        put_px(g, cx, my, INK); put_px(g, cx + 1, my, INK);
    }

    /* Sweat drop when weak, working out, or running */
    if (str_tier == 0 || s_act == ACT_TRAIN || s_act == ACT_RUN) {
        put_px(g, cx + hw, top_y + 2, CYAN);
        put_px(g, cx + hw, top_y + 3, CYAN);
    }

    /* Arms & props */
    int axL = cx - hw - 1;
    int axR = cx + 2 + hw;
    int ay  = top_y + 4;

    if (s_act == ACT_CHEER) {
        /* Raised arms + fluffy pink/cyan pom-poms + gold sparkles */
        int py = (phase & 1) ? top_y - 1 : top_y - 2;
        put_px(g, axL, top_y + 1, INK); put_px(g, axR, top_y + 1, INK);
        for (int dy = 0; dy <= 1; dy++) {
            for (int dx = -1; dx <= 0; dx++) {
                put_px(g, axL + dx, py + dy, PINK);
                put_px(g, axR - dx, py + dy, CYAN);
            }
        }
        put_px(g, (phase & 1) ? 1 : 2, 1, GOLD);
        put_px(g, (phase & 1) ? 16 : 15, 1, GOLD);
    } else if (s_act == ACT_EAT) {
        /* Cute pixel rice-ball / snack near mouth */
        int sx = cx + hw + 1;
        int sy = my - (phase & 1);
        put_px(g, sx, sy - 1, WHT);
        put_px(g, sx - 1, sy, WHT); put_px(g, sx, sy, WHT); put_px(g, sx + 1, sy, WHT);
        put_px(g, sx, sy, INK);   /* nori wrap */
        put_px(g, axL, ay, INK);
    } else if (s_act == ACT_TRAIN) {
        /* Overhead barbell / dumbbell lift! */
        int by = (phase & 1) ? 1 : 2;
        for (int x = cx - 4; x <= cx + 5; x++) put_px(g, x, by, IRON);
        put_px(g, cx - 5, by - 1, RED); put_px(g, cx - 5, by, RED); put_px(g, cx - 5, by + 1, RED);
        put_px(g, cx + 6, by - 1, RED); put_px(g, cx + 6, by, RED); put_px(g, cx + 6, by + 1, RED);
        put_px(g, cx - 3, by + 1, INK); put_px(g, cx + 4, by + 1, INK);
    } else if (s_act == ACT_RUN) {
        /* Pumping runner arms + speed lines */
        int ldy = (phase & 1) ? -1 : 1;
        int rdy = -ldy;
        put_px(g, axL, ay + ldy, INK); put_px(g, axL - 1, ay + ldy, BODY);
        put_px(g, axR, ay + rdy, INK); put_px(g, axR + 1, ay + rdy, BODY);
        put_px(g, 1, top_y + 2, CYAN); put_px(g, 2, top_y + 2, CYAN);
        put_px(g, 0, top_y + 5, GOLD); put_px(g, 1, top_y + 5, GOLD);
    } else {
        /* Idle arms reflect strength tier */
        if (str_tier == 2) {
            /* Buff flexed biceps! */
            put_px(g, axL,     ay,     BODY); put_px(g, axL - 1, ay,     INK);
            put_px(g, axL - 1, ay - 1, BODY); put_px(g, axL - 1, ay - 2, INK);
            put_px(g, axR,     ay,     BODY); put_px(g, axR + 1, ay,     INK);
            put_px(g, axR + 1, ay - 1, BODY); put_px(g, axR + 1, ay - 2, INK);
            put_px(g, 1, 2, GOLD); put_px(g, 16, 2, GOLD);
        } else if (str_tier == 0) {
            /* Weak drooping arms */
            put_px(g, axL, ay + 1, INK); put_px(g, axL, ay + 2, INK);
            put_px(g, axR, ay + 1, INK); put_px(g, axR, ay + 2, INK);
        } else {
            /* Normal cute arms holding small pom-poms */
            put_px(g, axL, ay, PINK); put_px(g, axL - 1, ay, PINK);
            put_px(g, axR, ay, CYAN); put_px(g, axR + 1, ay, CYAN);
        }
    }

    /* Upscale 18x14 grid by PIX_SCALE into PSRAM canvas */
    for (int sy = 0; sy < SPR_H; sy++) {
        for (int sx = 0; sx < SPR_W; sx++) {
            uint16_t col = g[sy * SPR_W + sx];
            int y0 = sy * PIX_SCALE;
            int x0 = sx * PIX_SCALE;
            for (int dy = 0; dy < PIX_SCALE; dy++) {
                uint16_t *dst = &s_buf[(size_t)(y0 + dy) * CAN_W + x0];
                for (int dx = 0; dx < PIX_SCALE; dx++) dst[dx] = col;
            }
        }
    }
    lv_obj_invalidate(s_canvas);

    if (s_shadow_gnd) {
        int sw = (body_tier == 0) ? 72 : (body_tier == 2 ? 124 : 96);
        if (s_act == ACT_CHEER) sw -= 18;
        lv_obj_set_size(s_shadow_gnd, sw, s_act == ACT_CHEER ? 10 : 14);
    }
}

static void update_status_ui(void)
{
    if (s_stat_lbl) {
        char buf[40];
        snprintf(buf, sizeof(buf), "FOOD %d  |  STR %d",
                 (int)s_pet.hunger, (int)s_pet.fitness);
        lv_label_set_text(s_stat_lbl, buf);
    }
    if (s_state_lbl) {
        const char *b = (s_pet.hunger < 30) ? "SKINNY" : (s_pet.hunger > 70 ? "CHUBBY" : "FIT");
        const char *s = (s_pet.fitness < 30) ? "WEAK"  : (s_pet.fitness > 70 ? "BUFF"   : "OK");
        char buf[32];
        snprintf(buf, sizeof(buf), "%s  -  %s", b, s);
        lv_label_set_text(s_state_lbl, buf);
        uint32_t col = 0x8E96B0;
        if (s_pet.hunger < 30) col = 0xFF8C42;
        else if (s_pet.fitness > 70) col = 0xFFD23F;
        else if (s_pet.hunger > 70) col = 0xFF6B9D;
        lv_obj_set_style_text_color(s_state_lbl, lv_color_hex(col), 0);
    }
}

static void set_bubble(const char *txt, uint32_t accent_hex, bool pop)
{
    if (s_cheer_lbl && txt) lv_label_set_text(s_cheer_lbl, txt);
    if (s_bubble_sh && accent_hex) {
        lv_obj_set_style_bg_color(s_bubble_sh, lv_color_hex(accent_hex), 0);
    }
    int dy = pop ? -118 : -112;
    if (s_bubble_sh) lv_obj_align(s_bubble_sh, LV_ALIGN_CENTER, 5, dy + 5);
    if (s_bubble)    lv_obj_align(s_bubble,    LV_ALIGN_CENTER, 0, dy);
    if (s_tail_out)  lv_obj_align(s_tail_out,  LV_ALIGN_CENTER, -14, dy + 40);
    if (s_tail_in)   lv_obj_align(s_tail_in,   LV_ALIGN_CENTER, -14, dy + 36);
}

static void do_cheer(void)
{
    s_pet.cheers++;
    uint32_t next = (s_last_cheer + 1 + ((lv_tick_get() ^ (s_pet.cheers * 7u)) % (CHEER_N - 1))) % CHEER_N;
    s_last_cheer = next;

    s_act = ACT_CHEER;
    s_act_ticks = 26;
    set_bubble(CHEERS[next], (s_pet.cheers & 1) ? 0xFF5C8A : 0xFFD23F, true);
    draw_pet(0);
    tone_play(CHEER_MELS[s_pet.cheers % 3]);
    pet_save();
}

static void do_feed(void)
{
    bool already_full = (s_pet.hunger >= 90);
    s_pet.hunger = clamp100(s_pet.hunger + 18);
    s_act = ACT_EAT;
    s_act_ticks = 22;

    const char *msg;
    if (already_full) {
        msg = FULL_LINES[(s_last_eat++) % (sizeof(FULL_LINES) / sizeof(FULL_LINES[0]))];
    } else {
        s_last_eat = (s_last_eat + 1) % EAT_N;
        msg = EAT_LINES[s_last_eat];
    }
    set_bubble(msg, 0xFF8C42, true);
    update_status_ui();
    draw_pet(0);
    tone_play(MEL_EAT);
    pet_save();
}

static void do_train(void)
{
    if (s_pet.hunger < 12) {
        s_act = ACT_IDLE;
        s_act_ticks = 14;
        set_bubble("TOO HUNGRY!", 0xFF5C5C, true);
        tone_play(MEL_UH_OH);
        return;
    }
    s_pet.fitness = clamp100(s_pet.fitness + 16);
    s_pet.hunger  = clamp100(s_pet.hunger - 10);
    s_act = ACT_TRAIN;
    s_act_ticks = 26;

    const char *msg = TRAIN_LINES[s_tick % (sizeof(TRAIN_LINES) / sizeof(TRAIN_LINES[0]))];
    set_bubble(msg, 0x2EC4B6, true);
    update_status_ui();
    draw_pet(0);
    tone_play(MEL_TRAIN);
    pet_save();
}

static void feed_btn_cb(lv_event_t *e)  { (void)e; do_feed();  }
static void train_btn_cb(lv_event_t *e) { (void)e; do_train(); }

static void trigger_run(void)
{
    bool fresh = (s_act != ACT_RUN);
    s_act = ACT_RUN;
    s_act_ticks = 30;   /* keep running ~1.5 s after last motion */
    lv_display_trigger_activity(NULL);

    if (fresh) {
        const char *msg = RUN_LINES[s_tick % (sizeof(RUN_LINES) / sizeof(RUN_LINES[0]))];
        set_bubble(msg, 0x5CE1E6, true);
        if (s_run_sound_cd == 0) {
            tone_play(MEL_RUN);
            s_run_sound_cd = 24;   /* >=1.2 s between run chimes */
        }
    }
}

static void bg_tap_cb(lv_event_t *e)
{
    if (lv_event_get_target(e) != lv_event_get_current_target(e)) return;
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        lv_indev_t *in = lv_indev_active();
        if (in) lv_indev_get_point(in, &s_press_pt);
        else    s_press_pt.x = s_press_pt.y = 0;
        return;
    }
    if (code == LV_EVENT_RELEASED) {
        /* Ignore drags >= 60 px and touches in the bottom button zone */
        lv_indev_t *in = lv_indev_active();
        lv_point_t p = s_press_pt;
        if (in) lv_indev_get_point(in, &p);
        if (s_press_pt.y >= 316 || p.y >= 316) return;
        int dx = p.x - s_press_pt.x, dy = p.y - s_press_pt.y;
        if (dx * dx + dy * dy >= 60 * 60) return;
        do_cheer();
    }
}

static void anim_cb(lv_timer_t *t)
{
    (void)t;
    /* Skip work while display is off — never call lv_timer_set_period() here */
    if (launcher_screen_is_off()) {
        tone_stop();
        return;
    }

    s_tick++;
    tone_step();
    if (s_run_sound_cd > 0) s_run_sound_cd--;

    /* Sense acceleration change (mg) -> start running immediately */
    float ax, ay, az;
    if (port_imu_accel3(&ax, &ay, &az)) {
        if (s_have_imu) {
            float dx = ax - s_prev_ax;
            float dy = ay - s_prev_ay;
            float dz = az - s_prev_az;
            if (dx * dx + dy * dy + dz * dz >= 220.0f * 220.0f
                && s_act != ACT_EAT && s_act != ACT_TRAIN) {
                trigger_run();
            }
        }
        s_prev_ax = ax; s_prev_ay = ay; s_prev_az = az;
        s_have_imu = true;
    }

    /* Running steadily builds a little fitness and burns a little food */
    if (s_act == ACT_RUN) {
        s_run_fit_acc++;
        if (s_run_fit_acc >= 20) {
            s_run_fit_acc = 0;
            s_pet.fitness = clamp100(s_pet.fitness + 2);
            s_pet.hunger  = clamp100(s_pet.hunger - 1);
            update_status_ui();
        }
    } else {
        s_run_fit_acc = 0;
    }

    /* Gentle in-app Tamagotchi decay: -1 hunger / 25 s, -1 fitness / 50 s */
    if ((s_tick % 500) == 0) {
        bool changed = false;
        if (s_pet.hunger > 0) { s_pet.hunger--; changed = true; }
        if ((s_tick % 1000) == 0 && s_pet.fitness > 0) { s_pet.fitness--; changed = true; }
        if (changed) {
            update_status_ui();
            if (s_pet.hunger < 25 && s_act == ACT_IDLE) {
                set_bubble(HUNGRY_LINES[s_tick % 3], 0xFF8C42, false);
            }
            pet_save();
        }
    }

    if (s_act_ticks > 0) {
        s_act_ticks--;
        if (s_act_ticks == 22) set_bubble(NULL, 0, false);
        int div = (s_act == ACT_RUN) ? 2 : 3;
        draw_pet((s_act_ticks / div) & 1);
        if (s_act_ticks == 0) s_act = ACT_IDLE;
    } else {
        draw_pet((s_tick / 12) & 1);
    }
}

static lv_obj_t *make_btn(lv_obj_t *parent, const char *label, uint32_t bg_hex,
                          int dx, int dy, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 112, 46);
    lv_obj_set_style_radius(b, 23, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg_hex), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(0x14141A), 0);
    lv_obj_set_style_border_width(b, 3, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0xFFFFFF), LV_STATE_PRESSED);
    lv_obj_align(b, LV_ALIGN_CENTER, dx, dy);
    lv_obj_set_ext_click_area(b, 14);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(b);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x14141A), 0);
    lv_obj_center(lbl);
    return b;
}

static void enter(lv_obj_t *root)
{
    port_crumb(CRUMB_GAME);
    pet_load();

    s_act = ACT_IDLE;
    s_act_ticks = 0;
    s_tick = 0;
    s_last_sig = 0xFFFFFFFFu;
    s_have_imu = false;
    s_run_sound_cd = 0;
    s_run_fit_acc = 0;
    s_melody = NULL;
    s_tone_active = false;

    /* Full-screen dark AMOLED canvas & tap catcher */
    lv_obj_t *bg = lv_obj_create(root);
    lv_obj_remove_style_all(bg);
    lv_obj_set_size(bg, 466, 466);
    lv_obj_center(bg);
    lv_obj_set_style_bg_color(bg, lv_color_hex(BG_HEX), 0);
    lv_obj_set_style_bg_opa(bg, LV_OPA_COVER, 0);
    lv_obj_add_flag(bg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(bg, bg_tap_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(bg, bg_tap_cb, LV_EVENT_RELEASED, NULL);

    /* Subtle pixel stars */
    static const int16_t STARS[][2] = {
        { -155, -66 }, { 158, -58 }, { -166, 36 }, { 166, 42 },
        { -136, -138 }, { 138, -134 },
    };
    for (size_t i = 0; i < sizeof(STARS) / sizeof(STARS[0]); i++) {
        lv_obj_t *st = lv_obj_create(bg);
        lv_obj_remove_style_all(st);
        lv_obj_set_size(st, (i & 1) ? 4 : 6, (i & 1) ? 4 : 6);
        lv_obj_set_style_bg_color(st, lv_color_hex((i & 1) ? 0x5CE1E6 : 0xFFE680), 0);
        lv_obj_set_style_bg_opa(st, 110, 0);
        lv_obj_align(st, LV_ALIGN_CENTER, STARS[i][0], STARS[i][1]);
        lv_obj_remove_flag(st, LV_OBJ_FLAG_CLICKABLE);
    }

    /* Top compact stat readout */
    s_stat_lbl = lv_label_create(bg);
    lv_obj_set_style_text_font(s_stat_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_stat_lbl, lv_color_hex(0xB8C0D8), 0);
    lv_obj_align(s_stat_lbl, LV_ALIGN_CENTER, 0, -172);

    /* ── Comic-book speech bubble ─────────────────────────────── */
    s_bubble_sh = lv_obj_create(bg);
    lv_obj_remove_style_all(s_bubble_sh);
    lv_obj_set_size(s_bubble_sh, 276, 74);
    lv_obj_set_style_radius(s_bubble_sh, 16, 0);
    lv_obj_set_style_bg_color(s_bubble_sh, lv_color_hex(0xFFD23F), 0);
    lv_obj_set_style_bg_opa(s_bubble_sh, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_bubble_sh, LV_OBJ_FLAG_CLICKABLE);

    s_tail_out = lv_obj_create(bg);
    lv_obj_remove_style_all(s_tail_out);
    lv_obj_set_size(s_tail_out, 22, 22);
    lv_obj_set_style_radius(s_tail_out, 4, 0);
    lv_obj_set_style_bg_color(s_tail_out, lv_color_hex(0x14141A), 0);
    lv_obj_set_style_bg_opa(s_tail_out, LV_OPA_COVER, 0);
    lv_obj_set_style_transform_rotation(s_tail_out, 450, 0);
    lv_obj_set_style_transform_pivot_x(s_tail_out, 11, 0);
    lv_obj_set_style_transform_pivot_y(s_tail_out, 11, 0);
    lv_obj_remove_flag(s_tail_out, LV_OBJ_FLAG_CLICKABLE);

    s_bubble = lv_obj_create(bg);
    lv_obj_remove_style_all(s_bubble);
    lv_obj_set_size(s_bubble, 276, 74);
    lv_obj_set_style_radius(s_bubble, 16, 0);
    lv_obj_set_style_bg_color(s_bubble, lv_color_hex(0xFFFDF2), 0);
    lv_obj_set_style_bg_opa(s_bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_bubble, lv_color_hex(0x14141A), 0);
    lv_obj_set_style_border_width(s_bubble, 4, 0);
    lv_obj_set_style_pad_hor(s_bubble, 12, 0);
    lv_obj_remove_flag(s_bubble, LV_OBJ_FLAG_CLICKABLE);

    s_tail_in = lv_obj_create(bg);
    lv_obj_remove_style_all(s_tail_in);
    lv_obj_set_size(s_tail_in, 16, 16);
    lv_obj_set_style_radius(s_tail_in, 2, 0);
    lv_obj_set_style_bg_color(s_tail_in, lv_color_hex(0xFFFDF2), 0);
    lv_obj_set_style_bg_opa(s_tail_in, LV_OPA_COVER, 0);
    lv_obj_set_style_transform_rotation(s_tail_in, 450, 0);
    lv_obj_set_style_transform_pivot_x(s_tail_in, 8, 0);
    lv_obj_set_style_transform_pivot_y(s_tail_in, 8, 0);
    lv_obj_remove_flag(s_tail_in, LV_OBJ_FLAG_CLICKABLE);

    s_cheer_lbl = lv_label_create(s_bubble);
    lv_obj_set_width(s_cheer_lbl, 248);
    lv_label_set_long_mode(s_cheer_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_cheer_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_cheer_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_cheer_lbl, lv_color_hex(0x14141A), 0);
    lv_obj_center(s_cheer_lbl);

    set_bubble(s_pet.hunger < 30 ? "SO HUNGRY..." : "READY TO CHEER!",
               s_pet.hunger < 30 ? 0xFF8C42 : 0xFFD23F, false);

    /* ── Tamagotchi ground shadow + pixel canvas ──────────────── */
    s_shadow_gnd = lv_obj_create(bg);
    lv_obj_remove_style_all(s_shadow_gnd);
    lv_obj_set_size(s_shadow_gnd, 96, 14);
    lv_obj_set_style_radius(s_shadow_gnd, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_shadow_gnd, lv_color_hex(0x23283B), 0);
    lv_obj_set_style_bg_opa(s_shadow_gnd, LV_OPA_COVER, 0);
    lv_obj_align(s_shadow_gnd, LV_ALIGN_CENTER, 0, 80);
    lv_obj_remove_flag(s_shadow_gnd, LV_OBJ_FLAG_CLICKABLE);

    s_buf = (uint16_t *)port_big_alloc((size_t)CAN_W * CAN_H * sizeof(uint16_t));
    if (s_buf) {
        s_canvas = lv_canvas_create(bg);
        lv_canvas_set_buffer(s_canvas, s_buf, CAN_W, CAN_H, LV_COLOR_FORMAT_RGB565);
        lv_obj_align(s_canvas, LV_ALIGN_CENTER, 0, 14);
        lv_obj_remove_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
        draw_pet(0);
    }

    /* ── Two custom buttons: FEED and TRAIN (tap pet to cheer) ── */
    make_btn(bg, "FEED",  0xFF8C42, -66, 126, feed_btn_cb);
    make_btn(bg, "TRAIN", 0x2EC4B6,  66, 126, train_btn_cb);

    /* Body & strength summary tag above the launcher handle */
    s_state_lbl = lv_label_create(bg);
    lv_obj_set_style_text_font(s_state_lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(s_state_lbl, LV_ALIGN_CENTER, 0, 174);

    update_status_ui();
    s_timer = lv_timer_create(anim_cb, 50, NULL);
}

static void leave(void)
{
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    tone_stop();
    pet_save();
    /* Delete LVGL canvas before freeing its PSRAM backing buffer */
    if (s_canvas) { lv_obj_delete(s_canvas); s_canvas = NULL; }
    if (s_buf)    { port_big_free(s_buf);    s_buf = NULL; }
    s_shadow_gnd = NULL;
    s_stat_lbl = NULL;
    s_state_lbl = NULL;
    s_bubble = NULL;
    s_bubble_sh = NULL;
    s_tail_out = NULL;
    s_tail_in = NULL;
    s_cheer_lbl = NULL;
}

static lv_color_t tint(void) { return lv_color_hex(0xFFE680); }

const badge_app_t app_cheer = {
    .name = "CheerLeader", .art = &app_icon_cheer, .icon = LV_SYMBOL_OK, .tint = tint,
    .radio = RADIO_OFF, .enter = enter, .leave = leave,
};
