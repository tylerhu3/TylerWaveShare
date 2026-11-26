/* Super Mouse: the computer on the USB cable runs the web app (source) and
 * sends mouse & 6KRO keyboard frames over USB serial; the badge forwards them
 * to its Bluetooth HID host (target). */
#include "app.h"
#include "assets/assets.h"
#include "port.h"
#include <stdio.h>
#include <string.h>

static lv_obj_t *s_link_btn;
static lv_obj_t *s_link_lbl;
static lv_obj_t *s_src_dot;
static lv_obj_t *s_src_lbl;
static lv_obj_t *s_act_lbl;
static lv_obj_t *s_hint_lbl;
static lv_obj_t *s_batt_lbl;
static lv_timer_t *s_poll;

static int s_prev_src;
static int s_prev_link;
static int s_prev_batt;
static int s_prev_plugged;
static uint8_t s_batt_skip;
static uint32_t s_prev_rx;
static char s_prev_evt[32];
static uint8_t s_off_skip;

/* Source states shown on the card (sm_state_t plus "no USB cable"). */
enum { SRC_UNPLUGGED = 100 };

static void link_btn_cb(lv_event_t *e) {
  (void)e;
  host_pick_open();
}

static void rel_btn_cb(lv_event_t *e) {
  (void)e;
  port_sm_release_all();
  if (s_act_lbl)
    lv_label_set_text(s_act_lbl, "released all keys");
}

static void poll_cb(lv_timer_t *t) {
  (void)t;
  /* Skip ticks while display is off — never call lv_timer_set_period() here. */
  if (launcher_screen_is_off()) {
    if (++s_off_skip % 4)
      return;
  }

  int link_now = port_hid_connected() ? 1 : 0;
  if (link_now != s_prev_link && s_link_lbl) {
    s_prev_link = link_now;
    const char *peer = port_hid_peer();
    lv_label_set_text_fmt(s_link_lbl, LV_SYMBOL_BLUETOOTH " %s",
                          link_now ? (peer && peer[0] ? peer : "connected")
                                   : "advertising - tap to pick");
    lv_obj_set_style_text_color(
        s_link_lbl, lv_color_hex(link_now ? 0x5BD48A : 0xE0B33A), 0);
  }

  /* VBUS is an I2C read; check it at ~1 Hz like the battery. */
  bool refresh_pmu = (s_batt_skip++ % 4) == 0;
  if (refresh_pmu)
    s_prev_plugged = port_battery_plugged() ? 1 : 0;

  sm_state_t st = port_sm_state();
  int src = (st == SM_WAITING && !s_prev_plugged) ? SRC_UNPLUGGED : (int)st;
  if (src != s_prev_src && s_src_lbl && s_src_dot) {
    s_prev_src = src;
    const char *txt = "Stopped";
    const char *hint = "";
    uint32_t col = 0x6E7686;
    switch (src) {
    case SRC_UNPLUGGED:
      txt = "Plug in USB";
      hint = "cable to the computer with the web app";
      col = 0xE0B33A;
      break;
    case SM_WAITING:
      txt = "Open web app";
      hint = "Chrome: localhost:8765 > Connect Badge";
      col = 0x38BDF8;
      break;
    case SM_LIVE:
      txt = "Web app live";
      hint = "USB computer  >  badge  >  BLE host";
      col = 0x5BD48A;
      break;
    case SM_ERR:
      txt = "Error";
      col = 0xFF6B6B;
      break;
    case SM_OFF:
    default:
      break;
    }
    lv_label_set_text(s_src_lbl, txt);
    lv_obj_set_style_text_color(s_src_lbl, lv_color_hex(col), 0);
    lv_obj_set_style_bg_color(s_src_dot, lv_color_hex(col), 0);
    if (s_hint_lbl)
      lv_label_set_text(s_hint_lbl, hint);
  }

  uint32_t rx = port_sm_rx_count();
  char last[32];
  port_sm_last_event(last, sizeof(last));
  if ((rx != s_prev_rx || strcmp(last, s_prev_evt) != 0) && s_act_lbl) {
    s_prev_rx = rx;
    snprintf(s_prev_evt, sizeof(s_prev_evt), "%s", last);
    lv_label_set_text_fmt(s_act_lbl, "rx %lu  |  %s", (unsigned long)rx, last);
  }

  if (s_batt_lbl && refresh_pmu) {
    int pct = port_battery_percent();
    if (pct >= 0 && pct != s_prev_batt) {
      s_prev_batt = pct;
      lv_label_set_text_fmt(s_batt_lbl, "BLE only  |  %d%%", pct);
    }
  }
}

static lv_obj_t *make_btn(lv_obj_t *root, int dx, int dy, int w, int h,
                          const char *txt, uint32_t bg_hex, lv_event_cb_t cb) {
  lv_obj_t *b = lv_button_create(root);
  lv_obj_set_size(b, w, h);
  lv_obj_set_style_radius(b, 20, 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(bg_hex), 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(0x4A7AA8), LV_STATE_PRESSED);
  lv_obj_set_style_border_width(b, 1, 0);
  lv_obj_set_style_border_color(b, lv_color_hex(0x3A4458), 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_align(b, LV_ALIGN_CENTER, dx, dy);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *l = lv_label_create(b);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(0xE8ECF0), 0);
  lv_obj_center(l);
  return b;
}

static void enter(lv_obj_t *root) {
  port_crumb(CRUMB_MOUSE);
  port_perf_hold(true);
  s_prev_src = -1;
  s_prev_link = -1;
  s_prev_batt = -2;
  s_prev_plugged = port_battery_plugged() ? 1 : 0;
  s_batt_skip = 1; /* plugged state was just read */
  s_prev_rx = 0xFFFFFFFFu;
  s_prev_evt[0] = '\0';
  s_off_skip = 0;

  lv_obj_t *title = lv_label_create(root);
  lv_label_set_text(title, "Super Mouse");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0xE8ECF0), 0);
  lv_obj_align(title, LV_ALIGN_CENTER, 0, -172);

  /* Target: the BLE host (tap to pick another paired host) */
  lv_obj_t *tgt = lv_label_create(root);
  lv_label_set_text(tgt, "TARGET");
  lv_obj_set_style_text_font(tgt, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(tgt, lv_color_hex(0x6E7686), 0);
  lv_obj_align(tgt, LV_ALIGN_CENTER, 0, -134);

  s_link_btn = lv_button_create(root);
  lv_obj_set_size(s_link_btn, 300, 40);
  lv_obj_set_style_radius(s_link_btn, 20, 0);
  lv_obj_set_style_bg_color(s_link_btn, lv_color_hex(0x1C202B), 0);
  lv_obj_set_style_bg_color(s_link_btn, lv_color_hex(0x2A3142),
                            LV_STATE_PRESSED);
  lv_obj_set_style_border_width(s_link_btn, 1, 0);
  lv_obj_set_style_border_color(s_link_btn, lv_color_hex(0x2E3548), 0);
  lv_obj_set_style_shadow_width(s_link_btn, 0, 0);
  lv_obj_align(s_link_btn, LV_ALIGN_CENTER, 0, -104);
  lv_obj_add_event_cb(s_link_btn, link_btn_cb, LV_EVENT_CLICKED, NULL);

  s_link_lbl = lv_label_create(s_link_btn);
  lv_obj_set_width(s_link_lbl, 276);
  lv_label_set_long_mode(s_link_lbl, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(s_link_lbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(s_link_lbl, &lv_font_montserrat_16, 0);
  lv_obj_center(s_link_lbl);

  /* Source: the USB computer running the web app */
  lv_obj_t *src = lv_label_create(root);
  lv_label_set_text(src, "SOURCE  " LV_SYMBOL_USB " USB");
  lv_obj_set_style_text_font(src, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(src, lv_color_hex(0x6E7686), 0);
  lv_obj_align(src, LV_ALIGN_CENTER, 0, -62);

  lv_obj_t *card = lv_obj_create(root);
  lv_obj_remove_style_all(card);
  lv_obj_set_size(card, 320, 78);
  lv_obj_set_style_radius(card, 20, 0);
  lv_obj_set_style_bg_color(card, lv_color_hex(0x181C24), 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_border_color(card, lv_color_hex(0x2B3344), 0);
  lv_obj_align(card, LV_ALIGN_CENTER, 0, -6);
  lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE);

  s_src_dot = lv_obj_create(card);
  lv_obj_remove_style_all(s_src_dot);
  lv_obj_set_size(s_src_dot, 14, 14);
  lv_obj_set_style_radius(s_src_dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(s_src_dot, LV_OPA_COVER, 0);
  lv_obj_align(s_src_dot, LV_ALIGN_LEFT_MID, 24, -12);

  s_src_lbl = lv_label_create(card);
  lv_obj_set_style_text_font(s_src_lbl, &lv_font_montserrat_24, 0);
  lv_obj_align(s_src_lbl, LV_ALIGN_LEFT_MID, 48, -12);

  s_act_lbl = lv_label_create(card);
  lv_obj_set_style_text_font(s_act_lbl, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(s_act_lbl, lv_color_hex(0x98A2B8), 0);
  lv_obj_align(s_act_lbl, LV_ALIGN_LEFT_MID, 24, 18);

  s_hint_lbl = lv_label_create(root);
  lv_obj_set_width(s_hint_lbl, 340);
  lv_label_set_long_mode(s_hint_lbl, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(s_hint_lbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(s_hint_lbl, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(s_hint_lbl, lv_color_hex(0x8A94A8), 0);
  lv_obj_align(s_hint_lbl, LV_ALIGN_CENTER, 0, 54);
  lv_label_set_text(s_hint_lbl, "");

  make_btn(root, 0, 104, 180, 48, "Release All", 0x3A2430, rel_btn_cb);

  s_batt_lbl = lv_label_create(root);
  lv_label_set_text(s_batt_lbl, "BLE only");
  lv_obj_set_style_text_font(s_batt_lbl, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(s_batt_lbl, lv_color_hex(0x6E7686), 0);
  lv_obj_align(s_batt_lbl, LV_ALIGN_CENTER, 0, 152);

  port_sm_start();
  s_poll = lv_timer_create(poll_cb, 250, NULL);
  poll_cb(NULL);
}

static void leave(void) {
  if (s_poll) {
    lv_timer_delete(s_poll);
    s_poll = NULL;
  }
  port_sm_stop();
  port_perf_hold(false);
  s_link_btn = NULL;
  s_link_lbl = NULL;
  s_src_dot = NULL;
  s_src_lbl = NULL;
  s_act_lbl = NULL;
  s_hint_lbl = NULL;
  s_batt_lbl = NULL;
}

static lv_color_t tint(void) { return lv_color_hex(0x38BDF8); }

const badge_app_t app_super_mouse = {
    .name = "Super Mouse",
    .art = &app_icon_super_mouse,
    .icon = LV_SYMBOL_USB,
    .tint = tint,
    .radio = RADIO_BLE,
    .keep_awake = true,
    .enter = enter,
    .leave = leave,
};
