/* Super Mouse: receives remote mouse & 6KRO keyboard commands over Wi-Fi
 * WebSocket from the Super Mouse backend and forwards them to either the
 * badge's Bluetooth HID host or native USB HID (OTG). */
#include "app.h"
#include "assets/assets.h"
#include "port.h"
#include <stdio.h>
#include <string.h>

static lv_obj_t *s_mode_ble;
static lv_obj_t *s_mode_usb;
static lv_obj_t *s_link_btn;
static lv_obj_t *s_link_lbl;
static lv_obj_t *s_ws_dot;
static lv_obj_t *s_ws_lbl;
static lv_obj_t *s_act_lbl;
static lv_obj_t *s_url_lbl;
static lv_obj_t *s_rel_lbl;
static lv_obj_t *s_batt_lbl;
static lv_timer_t *s_poll;

static int s_prev_sm;
static int s_prev_out;
static int s_prev_link;
static int s_prev_batt;
static int s_prev_plugged;
static int s_prev_usb_used;
static uint8_t s_batt_skip;
static uint32_t s_prev_rx;
static char s_prev_evt[32];
static char s_usb_err[48];
static uint8_t s_off_skip;

static void style_mode_btns(sm_out_t mode) {
  if (!s_mode_ble || !s_mode_usb)
    return;
  bool usb = (mode == SM_OUT_USB);
  lv_obj_set_style_bg_color(s_mode_ble, lv_color_hex(usb ? 0x161A24 : 0x244263),
                            0);
  lv_obj_set_style_border_color(s_mode_ble,
                                lv_color_hex(usb ? 0x2A3142 : 0x38BDF8), 0);
  lv_obj_set_style_bg_color(s_mode_usb, lv_color_hex(usb ? 0x1C4034 : 0x161A24),
                            0);
  lv_obj_set_style_border_color(s_mode_usb,
                                lv_color_hex(usb ? 0x5BD48A : 0x2A3142), 0);
}

static void refresh_url_label(void) {
  if (!s_url_lbl)
    return;
  char url[160];
  port_sm_url_get(url, sizeof(url));
  if (!url[0]) {
    lv_label_set_text(s_url_lbl, "tap Server (IP or IP:port)");
    lv_obj_set_style_text_color(s_url_lbl, lv_color_hex(0xE0B33A), 0);
    return;
  }
  /* Strip scheme prefix for compact display on the 466 px round screen. */
  const char *disp = url;
  if (strncmp(disp, "ws://", 5) == 0)
    disp += 5;
  else if (strncmp(disp, "wss://", 6) == 0)
    disp += 6;
  lv_label_set_text(s_url_lbl, disp);
  lv_obj_set_style_text_color(s_url_lbl, lv_color_hex(0x8A94A8), 0);
}

static void on_url_done(const char *text) {
  if (!text)
    return; /* cancelled */
  port_sm_url_set(text);
  refresh_url_label();
  port_sm_reconnect();
  s_prev_sm = -1;
}

static void link_btn_cb(lv_event_t *e) {
  (void)e;
  if (port_sm_output_get() == SM_OUT_BLE)
    host_pick_open();
}

static void mode_ble_cb(lv_event_t *e) {
  (void)e;
  s_usb_err[0] = '\0';
  if (port_sm_output_get() != SM_OUT_BLE)
    port_sm_output_set(SM_OUT_BLE);
  s_prev_out = -1;
  s_prev_link = -1;
  style_mode_btns(SM_OUT_BLE);
}

static void mode_usb_cb(lv_event_t *e) {
  (void)e;
  if (port_sm_output_get() == SM_OUT_USB)
    return;
  if (!port_sm_output_set(SM_OUT_USB)) {
    port_sm_usb_error(s_usb_err, sizeof(s_usb_err));
    if (!s_usb_err[0])
      snprintf(s_usb_err, sizeof(s_usb_err), "init failed");
  } else {
    s_usb_err[0] = '\0';
  }
  s_prev_out = -1;
  s_prev_link = -1;
  style_mode_btns(port_sm_output_get());
}

static void url_btn_cb(lv_event_t *e) {
  (void)e;
  char cur[160];
  port_sm_url_get(cur, sizeof(cur));
  keypad_open("Server IP:Port", cur, on_url_done);
}

static void rel_btn_cb(lv_event_t *e) {
  (void)e;
  sm_state_t st = port_sm_state();
  if (st != SM_CONNECTED) {
    port_sm_reconnect();
    s_prev_sm = -1;
    return;
  }
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

  sm_out_t out = port_sm_output_get();
  int link_now = (out == SM_OUT_USB) ? (port_sm_usb_mounted() ? 1 : 0)
                                     : (port_hid_connected() ? 1 : 0);
  if (((int)out != s_prev_out || link_now != s_prev_link) && s_link_lbl) {
    s_prev_out = (int)out;
    s_prev_link = link_now;
    style_mode_btns(out);
    if (out != SM_OUT_USB && s_usb_err[0]) {
      lv_label_set_text_fmt(s_link_lbl, "USB err: %s", s_usb_err);
      lv_obj_set_style_text_color(s_link_lbl, lv_color_hex(0xFF6B6B), 0);
      if (s_link_btn)
        lv_obj_set_style_border_color(s_link_btn, lv_color_hex(0xFF6B6B), 0);
    } else if (out == SM_OUT_USB) {
      lv_label_set_text(s_link_lbl,
                        link_now ? "USB: connected" : "USB: plug into PC");
      lv_obj_set_style_text_color(
          s_link_lbl, lv_color_hex(link_now ? 0x5BD48A : 0xE0B33A), 0);
      if (s_link_btn)
        lv_obj_set_style_border_color(s_link_btn, lv_color_hex(0x2E3548), 0);
    } else {
      const char *peer = port_hid_peer();
      lv_label_set_text_fmt(s_link_lbl, "BLE: %s",
                            link_now ? (peer && peer[0] ? peer : "connected")
                                     : "advertising");
      lv_obj_set_style_text_color(
          s_link_lbl, lv_color_hex(link_now ? 0x5BD48A : 0xE0B33A), 0);
      if (s_link_btn)
        lv_obj_set_style_border_color(s_link_btn, lv_color_hex(0x2E3548), 0);
    }
  }

  sm_state_t st = port_sm_state();
  if ((int)st != s_prev_sm && s_ws_lbl && s_ws_dot) {
    s_prev_sm = (int)st;
    const char *txt = "Off";
    uint32_t col = 0x6E7686;
    switch (st) {
    case SM_OFF:
      txt = "Stopped";
      col = 0x6E7686;
      break;
    case SM_WIFI_CONNECTING:
      txt = "Joining Wi-Fi";
      col = 0xE0B33A;
      break;
    case SM_WS_CONNECTING:
      txt = "Connecting WS";
      col = 0x38BDF8;
      break;
    case SM_CONNECTED:
      txt = "Bridge Live";
      col = 0x5BD48A;
      break;
    case SM_NO_URL:
      txt = "Set Server URL";
      col = 0xE0B33A;
      break;
    case SM_NO_WIFI:
      txt = "No Wi-Fi Saved";
      col = 0xFF6B6B;
      break;
    case SM_ERR:
      txt = "Connection Err";
      col = 0xFF6B6B;
      break;
    }
    lv_label_set_text(s_ws_lbl, txt);
    lv_obj_set_style_text_color(s_ws_lbl, lv_color_hex(col), 0);
    lv_obj_set_style_bg_color(s_ws_dot, lv_color_hex(col), 0);
    if (s_rel_lbl)
      lv_label_set_text(s_rel_lbl, st == SM_CONNECTED ? "Release" : "Retry");
  }

  uint32_t rx = port_sm_rx_count();
  char last[32];
  port_sm_last_event(last, sizeof(last));
  if ((rx != s_prev_rx || strcmp(last, s_prev_evt) != 0) && s_act_lbl) {
    s_prev_rx = rx;
    snprintf(s_prev_evt, sizeof(s_prev_evt), "%s", last);
    lv_label_set_text_fmt(s_act_lbl, "rx %lu  |  %s", (unsigned long)rx, last);
  }

  if (s_batt_lbl) {
    int usb_used = port_sm_usb_used() ? 1 : 0;
    if (usb_used) {
      if (usb_used != s_prev_usb_used || (int)out != s_prev_batt) {
        s_prev_usb_used = usb_used;
        s_prev_batt = (int)out;
        lv_label_set_text_fmt(s_batt_lbl, "Wi-Fi + %s  |  exit restarts",
                              out == SM_OUT_USB ? "USB" : "BLE");
      }
    } else if (s_prev_usb_used != 0 || (s_batt_skip++ % 8) == 0) {
      s_prev_usb_used = 0;
      int pct = port_battery_percent();
      int plugged = port_battery_plugged() ? 1 : 0;
      if (pct >= 0 && (pct != s_prev_batt || plugged != s_prev_plugged)) {
        s_prev_batt = pct;
        s_prev_plugged = plugged;
        lv_label_set_text_fmt(s_batt_lbl, "Wi-Fi + BLE  |  %d%%%s", pct,
                              plugged ? " USB" : "");
      }
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
  s_prev_sm = -1;
  s_prev_out = -1;
  s_prev_link = -1;
  s_prev_batt = -2;
  s_prev_plugged = -1;
  s_prev_usb_used = -1;
  s_batt_skip = 0;
  s_prev_rx = 0xFFFFFFFFu;
  s_prev_evt[0] = '\0';
  s_usb_err[0] = '\0';
  s_off_skip = 0;

  lv_obj_t *title = lv_label_create(root);
  lv_label_set_text(title, "Super Mouse");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0xE8ECF0), 0);
  lv_obj_align(title, LV_ALIGN_CENTER, 0, -172);

  /* BLE / USB output mode switch */
  s_mode_ble = make_btn(root, -54, -124, 100, 38, LV_SYMBOL_BLUETOOTH " BLE",
                        0x244263, mode_ble_cb);
  s_mode_usb = make_btn(root, 54, -124, 100, 38, LV_SYMBOL_USB " USB", 0x161A24,
                        mode_usb_cb);
  style_mode_btns(port_sm_output_get());

  /* Active transport status pill (tapping opens host picker in BLE mode) */
  s_link_btn = lv_button_create(root);
  lv_obj_set_size(s_link_btn, 256, 34);
  lv_obj_set_style_radius(s_link_btn, 17, 0);
  lv_obj_set_style_bg_color(s_link_btn, lv_color_hex(0x1C202B), 0);
  lv_obj_set_style_border_width(s_link_btn, 1, 0);
  lv_obj_set_style_border_color(s_link_btn, lv_color_hex(0x2E3548), 0);
  lv_obj_set_style_shadow_width(s_link_btn, 0, 0);
  lv_obj_align(s_link_btn, LV_ALIGN_CENTER, 0, -76);
  lv_obj_add_event_cb(s_link_btn, link_btn_cb, LV_EVENT_CLICKED, NULL);

  s_link_lbl = lv_label_create(s_link_btn);
  lv_obj_set_width(s_link_lbl, 236);
  lv_label_set_long_mode(s_link_lbl, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(s_link_lbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(s_link_lbl, &lv_font_montserrat_16, 0);
  lv_obj_center(s_link_lbl);

  /* Central WebSocket bridge status card */
  lv_obj_t *card = lv_obj_create(root);
  lv_obj_remove_style_all(card);
  lv_obj_set_size(card, 320, 78);
  lv_obj_set_style_radius(card, 20, 0);
  lv_obj_set_style_bg_color(card, lv_color_hex(0x181C24), 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_border_color(card, lv_color_hex(0x2B3344), 0);
  lv_obj_align(card, LV_ALIGN_CENTER, 0, -10);
  lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE);

  s_ws_dot = lv_obj_create(card);
  lv_obj_remove_style_all(s_ws_dot);
  lv_obj_set_size(s_ws_dot, 14, 14);
  lv_obj_set_style_radius(s_ws_dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(s_ws_dot, LV_OPA_COVER, 0);
  lv_obj_align(s_ws_dot, LV_ALIGN_LEFT_MID, 24, -12);

  s_ws_lbl = lv_label_create(card);
  lv_obj_set_style_text_font(s_ws_lbl, &lv_font_montserrat_24, 0);
  lv_obj_align(s_ws_lbl, LV_ALIGN_LEFT_MID, 48, -12);

  s_act_lbl = lv_label_create(card);
  lv_obj_set_style_text_font(s_act_lbl, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(s_act_lbl, lv_color_hex(0x98A2B8), 0);
  lv_obj_align(s_act_lbl, LV_ALIGN_LEFT_MID, 24, 18);

  /* Truncated server URL display */
  s_url_lbl = lv_label_create(root);
  lv_obj_set_width(s_url_lbl, 330);
  lv_label_set_long_mode(s_url_lbl, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(s_url_lbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(s_url_lbl, &lv_font_montserrat_16, 0);
  lv_obj_align(s_url_lbl, LV_ALIGN_CENTER, 0, 50);
  refresh_url_label();

  /* Action buttons: Server URL edit + Release All / Retry */
  make_btn(root, -82, 100, 148, 48, "Server", 0x24344D, url_btn_cb);
  lv_obj_t *rel_btn =
      make_btn(root, 82, 100, 148, 48, "Retry", 0x3A2430, rel_btn_cb);
  s_rel_lbl = lv_obj_get_child(rel_btn, 0);

  /* Footer battery + mode readout (above the home handle) */
  s_batt_lbl = lv_label_create(root);
  lv_label_set_text(s_batt_lbl, "Wi-Fi + BLE");
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
  port_sm_leave();
  port_perf_hold(false);
  s_mode_ble = NULL;
  s_mode_usb = NULL;
  s_link_btn = NULL;
  s_link_lbl = NULL;
  s_ws_dot = NULL;
  s_ws_lbl = NULL;
  s_act_lbl = NULL;
  s_url_lbl = NULL;
  s_rel_lbl = NULL;
  s_batt_lbl = NULL;
}

static lv_color_t tint(void) { return lv_color_hex(0x38BDF8); }

const badge_app_t app_super_mouse = {
    .name = "Super Mouse",
    .art = &app_icon_super_mouse,
    .icon = LV_SYMBOL_WIFI,
    .tint = tint,
    .radio = RADIO_BLE,
    .keep_awake = true,
    .enter = enter,
    .leave = leave,
};
