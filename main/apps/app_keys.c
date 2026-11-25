/* The F-key dial.
 *
 * A phone's soft keyboard has no F keys. That is the real reason Excel
 * formulas cannot be touched over RDP from a phone — without F4 (toggling
 * absolute references) there is no building a formula chain. They are laid out
 * radially on the round screen and fired on a press.
 *
 * Unlike Apple's Touch Bar: that took away physical F keys people already had,
 * and was hated for it. This adds ones that were never there. */
#include "app.h"
#include "assets/assets.h"
#include "port.h"
#include <math.h>

/* USB HID key codes */
#define K_F1   0x3A
#define K_F2   0x3B
#define K_F4   0x3D
#define K_F5   0x3E
#define K_F12  0x45
#define K_ESC  0x29
#define K_TAB  0x2B
#define K_ENT  0x28
#define K_SEMI 0x33      /* ; */
#define K_LBRK 0x2F      /* [ */
#define K_DEL  0x4C

#define MOD_CTRL   1
#define MOD_SHIFT  2

typedef struct {
    const char *label;
    unsigned    mod;
    unsigned    code;
    const char *hint;
} key_t;

/* The ones Excel reaches for most. The order runs clockwise from 12 o'clock. */
static const key_t KEYS[] = {
    { "F4",  0,         K_F4,   "absolute ref" },
    { "F2",  0,         K_F2,   "edit cell" },
    { "F5",  0,         K_F5,   "go to" },
    { "F12", 0,         K_F12,  "save as" },
    { "Esc", 0,         K_ESC,  "cancel" },
    { "Tab", 0,         K_TAB,  "" },
    { "Ent", 0,         K_ENT,  "" },
    { "^;",  MOD_CTRL,  K_SEMI, "today" },
    { "^[",  MOD_CTRL,  K_LBRK, "trace prec" },
    { "Del", 0,         K_DEL,  "" },
    { "F1",  0,         K_F1,   "help" },
    { "^F4", MOD_CTRL,  K_F4,   "close" },
};
#define KEY_CNT (sizeof(KEYS) / sizeof(KEYS[0]))

static lv_obj_t   *s_hint, *s_state;
static lv_timer_t *s_poll;

static void key_cb(lv_event_t *e)
{
    const key_t *k = (const key_t *)lv_event_get_user_data(e);
    port_hid_key(k->mod, k->code);
    lv_label_set_text_fmt(s_hint, "%s%s%s", k->label,
                          k->hint[0] ? "  " : "", k->hint);
}

static void poll_cb(lv_timer_t *t)
{
    /* 🔋 Screen off, nobody is looking. But 🚨 the period must not be changed
     * here — lv_timer_set_period() calls lv_timer_handler_resume() internally,
     * so calling it from a timer callback sends the handler round again on the
     * spot, forever.
     * (09-09: added to save power, and it pinned the CPU at 100%. Setting the
     *  same value does it too, so "only set it when it changes" does not help.)
     * The period stays as it is and the work happens on one tick in four. Same
     * effect, and safe. */
    if (launcher_screen_is_off()) {
        static uint8_t skip;
        if (++skip % 4) return;
    }

    /* Nothing to redraw while the connection state is unchanged. LVGL
     * invalidates unconditionally when a style is set, same value or not. */
    static int s_prev = -1;
    int now = port_hid_connected() ? 1 : 0;
    if (now == s_prev) return;
    s_prev = now;
    bool on = port_hid_connected();
    lv_label_set_text(s_state, on ? "" : "not connected");
    lv_obj_set_style_text_color(s_state, lv_color_hex(0xE0B33A), 0);
}

static void fkeys_build(lv_obj_t *root)
{
    /* Twelve of them, 30 degrees apart. The middle is left clear to show what was pressed. */
    for (unsigned i = 0; i < KEY_CNT; i++) {
        float ang = (float)(-M_PI / 2.0 + i * (2.0 * M_PI / KEY_CNT));
        int x = (int)(cosf(ang) * 168.f);
        int y = (int)(sinf(ang) * 168.f);

        lv_obj_t *b = lv_button_create(root);
        lv_obj_set_size(b, 76, 60);
        lv_obj_set_style_radius(b, 18, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(KEYS[i].mod ? 0x2E4A66 : 0x24242A), 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x4A7AA8), LV_STATE_PRESSED);
        lv_obj_align(b, LV_ALIGN_CENTER, x, y);
        lv_obj_add_event_cb(b, key_cb, LV_EVENT_CLICKED, (void *)&KEYS[i]);

        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, KEYS[i].label);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
        lv_obj_center(l);
    }

    s_hint = lv_label_create(root);
    lv_label_set_text(s_hint, "Excel keys");
    lv_obj_set_style_text_font(s_hint, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(0xE8ECF0), 0);
    lv_obj_align(s_hint, LV_ALIGN_CENTER, 0, -12);

    s_state = lv_label_create(root);
    lv_obj_set_style_text_font(s_state, &lv_font_montserrat_16, 0);
    lv_obj_align(s_state, LV_ALIGN_CENTER, 0, 20);

    s_poll = lv_timer_create(poll_cb, 500, NULL);
    poll_cb(NULL);
}

/* ── horizontal scrolling keyboard (2 columns visible at a time) ── */
#define K_BKSP   0x2A
#define K_SPACE  0x2C
#define K_HOME   0x4A
#define K_END    0x4D
#define K_RIGHT  0x4F
#define K_LEFT   0x50
#define K_DOWN   0x51
#define K_UP     0x52

#define KBD_ROWS 4

typedef struct {
    const char *lo;       /* label without Shift */
    const char *hi;       /* label with Shift (NULL = same as lo) */
    uint8_t     code;     /* USB HID usage ID (page 0x07) */
    uint8_t     special;  /* 1 = navigation / control key */
} kbd_key_t;

/* Full US QWERTY + navigation laid out as columns of 4 rows.
 * Scrolling horizontally reveals 2 columns at a time (148 px each + 12 px gap). */
static const kbd_key_t KBD_COLS[][KBD_ROWS] = {
    { { "1", "!", 0x1E, 0 }, { "q", "Q", 0x14, 0 }, { "a", "A", 0x04, 0 }, { "z", "Z", 0x1D, 0 } },
    { { "2", "@", 0x1F, 0 }, { "w", "W", 0x1A, 0 }, { "s", "S", 0x16, 0 }, { "x", "X", 0x1B, 0 } },
    { { "3", "#", 0x20, 0 }, { "e", "E", 0x08, 0 }, { "d", "D", 0x07, 0 }, { "c", "C", 0x06, 0 } },
    { { "4", "$", 0x21, 0 }, { "r", "R", 0x15, 0 }, { "f", "F", 0x09, 0 }, { "v", "V", 0x19, 0 } },
    { { "5", "%", 0x22, 0 }, { "t", "T", 0x17, 0 }, { "g", "G", 0x0A, 0 }, { "b", "B", 0x05, 0 } },
    { { "6", "^", 0x23, 0 }, { "y", "Y", 0x1C, 0 }, { "h", "H", 0x0B, 0 }, { "n", "N", 0x11, 0 } },
    { { "7", "&", 0x24, 0 }, { "u", "U", 0x18, 0 }, { "j", "J", 0x0D, 0 }, { "m", "M", 0x10, 0 } },
    { { "8", "*", 0x25, 0 }, { "i", "I", 0x0C, 0 }, { "k", "K", 0x0E, 0 }, { ",", "<", 0x36, 0 } },
    { { "9", "(", 0x26, 0 }, { "o", "O", 0x12, 0 }, { "l", "L", 0x0F, 0 }, { ".", ">", 0x37, 0 } },
    { { "0", ")", 0x27, 0 }, { "p", "P", 0x13, 0 }, { ";", ":", 0x33, 0 }, { "/", "?", 0x38, 0 } },
    { { "-", "_", 0x2D, 0 }, { "[", "{", 0x2F, 0 }, { "'", "\"", 0x34, 0 }, { "`", "~", 0x35, 0 } },
    { { "=", "+", 0x2E, 0 }, { "]", "}", 0x30, 0 }, { "\\", "|", 0x31, 0 }, { "Tab", NULL, K_TAB, 1 } },
    { { "Esc",  NULL, K_ESC,  1 }, { LV_SYMBOL_UP,   NULL, K_UP,    1 },
      { LV_SYMBOL_LEFT, NULL, K_LEFT, 1 }, { "Del",  NULL, K_DEL,   1 } },
    { { "Home", NULL, K_HOME, 1 }, { LV_SYMBOL_DOWN, NULL, K_DOWN,  1 },
      { LV_SYMBOL_RIGHT, NULL, K_RIGHT, 1 }, { "End", NULL, K_END,  1 } },
};
#define KBD_COL_CNT (sizeof(KBD_COLS) / sizeof(KBD_COLS[0]))

static lv_obj_t *s_kbd_scroll, *s_kbd_state, *s_kbd_shift_btn;
static bool      s_kbd_shift;

static void kbd_refresh_labels(void)
{
    if (!s_kbd_scroll) return;
    for (unsigned c = 0; c < KBD_COL_CNT; c++) {
        lv_obj_t *col = lv_obj_get_child(s_kbd_scroll, (int32_t)c);
        if (!col) continue;
        for (unsigned r = 0; r < KBD_ROWS; r++) {
            lv_obj_t *btn = lv_obj_get_child(col, (int32_t)r);
            if (!btn) continue;
            lv_obj_t *lbl = lv_obj_get_child(btn, 0);
            if (!lbl) continue;
            const kbd_key_t *k = &KBD_COLS[c][r];
            const char *txt = (s_kbd_shift && k->hi) ? k->hi : k->lo;
            lv_label_set_text(lbl, txt);
        }
    }
    if (s_kbd_shift_btn) {
        lv_obj_set_style_bg_color(
            s_kbd_shift_btn,
            lv_color_hex(s_kbd_shift ? 0x3A6EA5 : 0x24242A), 0);
        lv_obj_t *lbl = lv_obj_get_child(s_kbd_shift_btn, 0);
        if (lbl) lv_label_set_text(lbl, s_kbd_shift ? "SHIFT" : "Shift");
    }
}

static void kbd_key_cb(lv_event_t *e)
{
    const kbd_key_t *k = (const kbd_key_t *)lv_event_get_user_data(e);
    unsigned mod = (s_kbd_shift && !k->special) ? MOD_SHIFT : 0;
    port_hid_key(mod, k->code);
    if (s_kbd_state) {
        const char *txt = (mod && k->hi) ? k->hi : k->lo;
        lv_obj_set_style_text_color(s_kbd_state, lv_color_hex(0x5BD48A), 0);
        lv_label_set_text_fmt(s_kbd_state, "sent  %s", txt);
    }
}

static void kbd_shift_cb(lv_event_t *e)
{
    (void)e;
    s_kbd_shift = !s_kbd_shift;
    kbd_refresh_labels();
}

static void kbd_action_cb(lv_event_t *e)
{
    const key_t *k = (const key_t *)lv_event_get_user_data(e);
    port_hid_key(k->mod, k->code);
    if (s_kbd_state) {
        lv_obj_set_style_text_color(s_kbd_state, lv_color_hex(0x5BD48A), 0);
        lv_label_set_text_fmt(s_kbd_state, "sent  %s", k->hint);
    }
}

static lv_obj_t *kbd_bar_btn(lv_obj_t *root, int dx, int w, const char *txt,
                             lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_button_create(root);
    lv_obj_set_size(b, w, 44);
    lv_obj_set_style_radius(b, 14, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x24242A), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x4A7AA8), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(0x3A3A46), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_align(b, LV_ALIGN_CENTER, dx, 152);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);

    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xE8ECF0), 0);
    lv_obj_center(l);
    return b;
}

static const key_t KBD_ACT_SPACE = { "Spc",  0, K_SPACE, "Space" };
static const key_t KBD_ACT_BKSP  = { "Bksp", 0, K_BKSP,  "Bksp"  };
static const key_t KBD_ACT_ENT   = { "Ent",  0, K_ENT,   "Enter" };

static void kbd_build(lv_obj_t *root)
{
    s_kbd_shift = false;

    lv_obj_t *t = lv_label_create(root);
    lv_label_set_text(t, "Keyboard");
    lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0x8A8A90), 0);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, -188);

    s_kbd_state = lv_label_create(root);
    lv_label_set_text(s_kbd_state, "< swipe >");
    lv_obj_set_style_text_font(s_kbd_state, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_kbd_state, lv_color_hex(0x6E7686), 0);
    lv_obj_align(s_kbd_state, LV_ALIGN_CENTER, 0, -158);

    /* Horizontal scroll viewport: 324 x 260 centered at (0, -10).
     * Inner width = 324 - 16 = 308 px = 2 * 148 px columns + 12 px gap.
     * Corners sit at hypot(162, 140) = 214 px < 233 px screen radius. */
    lv_obj_t *sc = lv_obj_create(root);
    s_kbd_scroll = sc;
    lv_obj_set_size(sc, 324, 260);
    lv_obj_align(sc, LV_ALIGN_CENTER, 0, -10);
    lv_obj_set_style_bg_opa(sc, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sc, 0, 0);
    lv_obj_set_style_pad_left(sc, 8, 0);
    lv_obj_set_style_pad_right(sc, 8, 0);
    lv_obj_set_style_pad_top(sc, 4, 0);
    lv_obj_set_style_pad_bottom(sc, 8, 0);
    lv_obj_set_style_pad_column(sc, 12, 0);
    lv_obj_set_flex_flow(sc, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sc, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(sc, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(sc, LV_SCROLL_SNAP_START);
    lv_obj_set_scrollbar_mode(sc, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(sc, lv_color_hex(0x6E7686), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(sc, LV_OPA_60, LV_PART_SCROLLBAR);

    for (unsigned c = 0; c < KBD_COL_CNT; c++) {
        lv_obj_t *col = lv_obj_create(sc);
        lv_obj_remove_style_all(col);
        lv_obj_set_size(col, 148, 248);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(col, 8, 0);
        lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(col, LV_OBJ_FLAG_SNAPPABLE);

        for (unsigned r = 0; r < KBD_ROWS; r++) {
            const kbd_key_t *k = &KBD_COLS[c][r];
            lv_obj_t *b = lv_button_create(col);
            lv_obj_set_size(b, 148, 56);
            lv_obj_set_style_radius(b, 14, 0);
            lv_obj_set_style_bg_color(b, lv_color_hex(k->special ? 0x2E4A66 : 0x24242A), 0);
            lv_obj_set_style_bg_color(b, lv_color_hex(0x4A7AA8), LV_STATE_PRESSED);
            lv_obj_set_style_border_width(b, 1, 0);
            lv_obj_set_style_border_color(b, lv_color_hex(0x3A3A46), 0);
            lv_obj_set_style_shadow_width(b, 0, 0);
            lv_obj_add_event_cb(b, kbd_key_cb, LV_EVENT_CLICKED, (void *)k);

            lv_obj_t *l = lv_label_create(b);
            lv_label_set_text(l, k->lo);
            lv_obj_set_style_text_font(l, k->special ? &lv_font_montserrat_20
                                                     : &lv_font_montserrat_24, 0);
            lv_obj_set_style_text_color(l, lv_color_hex(0xE8ECF0), 0);
            lv_obj_center(l);
        }
    }

    /* Quick action bar below the scroll view (above the home handle). */
    s_kbd_shift_btn = kbd_bar_btn(root, -113, 68, "Shift", kbd_shift_cb, NULL);
    kbd_bar_btn(root,  -37, 72, "Space", kbd_action_cb, (void *)&KBD_ACT_SPACE);
    kbd_bar_btn(root,   39, 68, LV_SYMBOL_BACKSPACE, kbd_action_cb, (void *)&KBD_ACT_BKSP);
    kbd_bar_btn(root,  113, 68, "Ent",   kbd_action_cb, (void *)&KBD_ACT_ENT);
}

static void kbd_free(void)
{
    s_kbd_scroll = NULL;
    s_kbd_state = NULL;
    s_kbd_shift_btn = NULL;
    s_kbd_shift = false;
}

/* All four send keystrokes, so they are one app swiped vertically.
 * Easier to find that way than separate icons on the home screen. */
void type_build(lv_obj_t *root);
void present_build(lv_obj_t *root);
void present_free(void);

static void enter(lv_obj_t *root)
{
    lv_obj_t *tv = lv_tileview_create(root);
    lv_obj_set_size(tv, 466, 466);
    lv_obj_center(tv);
    lv_obj_set_style_bg_opa(tv, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(tv, LV_SCROLLBAR_MODE_OFF);

    fkeys_build(lv_tileview_add_tile(tv, 0, 0, LV_DIR_BOTTOM));
    kbd_build(lv_tileview_add_tile(tv, 0, 1, LV_DIR_TOP | LV_DIR_BOTTOM));
    type_build(lv_tileview_add_tile(tv, 0, 2, LV_DIR_TOP | LV_DIR_BOTTOM));
    present_build(lv_tileview_add_tile(tv, 0, 3, LV_DIR_TOP));
}

static void leave(void)
{
    if (s_poll) { lv_timer_delete(s_poll); s_poll = NULL; }
    kbd_free();
    present_free();
}

static lv_color_t tint(void) { return lv_color_hex(0x8AB4F8); }

const badge_app_t app_keys = {
    .name = "Keys", .art = &app_icon_keys, .icon = LV_SYMBOL_KEYBOARD, .tint = tint,
    /* This app is used while looking at it, so there is little ground for
     * holding the screen on indefinitely. Going dark after 30 seconds is one
     * press of PWR away — on versus off is 124 mV an hour. */
    .radio = RADIO_BLE, .keep_awake = false, .enter = enter, .leave = leave,
};
