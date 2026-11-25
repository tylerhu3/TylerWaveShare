/* Super Mouse network bridge (ESP32-S3 firmware only).
 *
 * Brings up Wi-Fi STA alongside BLE HID (SW coexistence), opens a persistent
 * WebSocket to the Super Mouse backend, and translates incoming binary HID
 * frames directly into BLE HID mouse and 6KRO keyboard reports.
 *
 * Binary frame protocol (client -> backend -> badge):
 *   0x01 Mouse:    [0x01, buttons_u8, dx_i16_le, dy_i16_le, wheel_i8]  (7 B)
 *   0x02 Keyboard: [0x02, modifiers_u8, k0, k1, k2, k3, k4, k5]        (8 B)
 *   0x03 KeyTap:   [0x03, modifiers_u8, k0, k1, k2, hold_ms_u8]        (6 B)
 *   0x04 Release:  [0x04]                                              (1 B)
 */
#include "esp_coexist.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "port.h"
#include "usb_hid.h"
#include <stdio.h>
#include <string.h>

extern void badge_ble_adv_pause(bool pause);

#if __has_include("secrets.h")
#include "secrets.h"
#endif

static const char *TAG = "super_mouse";

#define SM_NVS_NS "badge"
#define SM_NVS_KEY "sm_url"
#define SM_URL_MAX 160

static volatile sm_state_t s_state = SM_OFF;
static volatile sm_out_t s_output = SM_OUT_BLE;
static volatile bool s_stop_req;
static volatile bool s_restart_req;
static volatile bool s_task_running;
static volatile uint32_t s_rx_count;
static char s_last_evt[32] = "idle";
static esp_websocket_client_handle_t s_ws;
static esp_timer_handle_t s_macro_timer;

/* Lightweight hot-path event tag so mouse/key frames never call snprintf on
 * the WebSocket RX thread; formatted lazily in port_sm_last_event() at 4 Hz. */
typedef enum {
  EVT_TEXT = 0,
  EVT_MOVE,
  EVT_WHEEL,
  EVT_BTN,
  EVT_KEY,
  EVT_KEYS_UP,
  EVT_MACRO,
} evt_kind_t;
static volatile evt_kind_t s_evt_kind = EVT_TEXT;
static volatile int s_evt_a;
static volatile int s_evt_b;

static void send_kbd_out(uint8_t mod, const uint8_t keys[6]) {
  if (s_output == SM_OUT_USB)
    usb_hid_keyboard_report(mod, keys);
  else
    port_hid_keyboard_report(mod, keys);
}

static void macro_release_cb(void *arg) {
  (void)arg;
  send_kbd_out(0, NULL);
}

static void macro_timer_cancel(void) {
  if (s_macro_timer)
    esp_timer_stop(s_macro_timer);
}

void port_sm_release_all(void) {
  macro_timer_cancel();
  port_hid_release_all();
  usb_hid_release_all();
}

static void set_last_evt(const char *msg) {
  if (!msg)
    return;
  snprintf(s_last_evt, sizeof(s_last_evt), "%s", msg);
  s_evt_kind = EVT_TEXT;
}

/* Expand shorthand "192.168.2.1" or "192.168.2.1:8765" into a full WebSocket
 * URI so typing on the tiny multi-tap keypad only needs an IP (and optional
 * port). */
static void expand_ws_url(const char *raw, char *out, size_t len) {
  if (!raw || !raw[0] || !out || !len) {
    if (out && len)
      out[0] = '\0';
    return;
  }
  while (*raw == ' ' || *raw == '\t')
    raw++;
  char trimmed[SM_URL_MAX];
  snprintf(trimmed, sizeof(trimmed), "%s", raw);
  size_t tlen = strlen(trimmed);
  while (tlen > 0 && (trimmed[tlen - 1] == ' ' || trimmed[tlen - 1] == '\t'))
    trimmed[--tlen] = '\0';
  if (!trimmed[0]) {
    out[0] = '\0';
    return;
  }
  raw = trimmed;
  bool has_scheme =
      (strncmp(raw, "ws://", 5) == 0 || strncmp(raw, "wss://", 6) == 0);
  const char *host = has_scheme ? (strstr(raw, "://") + 3) : raw;
  bool has_path = (strchr(host, '/') != NULL);
  bool has_port = (strchr(host, ':') != NULL);

  if (has_scheme) {
    if (has_path)
      snprintf(out, len, "%s", raw);
    else
      snprintf(out, len, "%s/ws/device?room=badge1", raw);
  } else if (has_path) {
    snprintf(out, len, "ws://%s", raw);
  } else if (has_port) {
    snprintf(out, len, "ws://%s/ws/device?room=badge1", raw);
  } else {
    snprintf(out, len, "ws://%s:8765/ws/device?room=badge1", raw);
  }
}

void port_sm_url_get(char *out, size_t len) {
  if (!out || !len)
    return;
  out[0] = '\0';

  nvs_handle_t nh;
  if (nvs_open(SM_NVS_NS, NVS_READONLY, &nh) == ESP_OK) {
    size_t sz = len;
    if (nvs_get_str(nh, SM_NVS_KEY, out, &sz) != ESP_OK)
      out[0] = '\0';
    nvs_close(nh);
  }

#ifdef BADGE_SUPER_MOUSE_URL
  if (!out[0] && BADGE_SUPER_MOUSE_URL[0]) {
    snprintf(out, len, "%s", BADGE_SUPER_MOUSE_URL);
  }
#endif
}

void port_sm_url_set(const char *url) {
  nvs_handle_t nh;
  if (nvs_open(SM_NVS_NS, NVS_READWRITE, &nh) != ESP_OK)
    return;
  if (url && url[0])
    nvs_set_str(nh, SM_NVS_KEY, url);
  else
    nvs_erase_key(nh, SM_NVS_KEY);
  nvs_commit(nh);
  nvs_close(nh);
  ESP_LOGI(TAG, "saved server URL: %s", (url && url[0]) ? url : "(cleared)");
}

static void handle_mouse_frame(const uint8_t *d, size_t len) {
  if (len < 7)
    return;
  uint8_t buttons = d[1] & 0x07;
  int16_t dx = (int16_t)((uint16_t)d[2] | ((uint16_t)d[3] << 8));
  int16_t dy = (int16_t)((uint16_t)d[4] | ((uint16_t)d[5] << 8));
  int8_t wh = (int8_t)d[6];

  if (s_output == SM_OUT_USB) {
    /* USB HID uses 16-bit X/Y — send the entire delta in a single report. */
    usb_hid_mouse(dx, dy, buttons, wh);
  } else {
    /* BLE HID report map uses 8-bit X/Y; chunk large deltas into [-127, 127].
     */
    bool first = true;
    do {
      int sx = dx > 127 ? 127 : (dx < -127 ? -127 : dx);
      int sy = dy > 127 ? 127 : (dy < -127 ? -127 : dy);
      port_hid_mouse(sx, sy, buttons, first ? (int)wh : 0);
      dx -= (int16_t)sx;
      dy -= (int16_t)sy;
      first = false;
    } while (dx != 0 || dy != 0);
  }

  if (wh) {
    s_evt_a = (int)wh;
    s_evt_kind = EVT_WHEEL;
  } else if (buttons) {
    s_evt_a = (int)buttons;
    s_evt_kind = EVT_BTN;
  } else {
    s_evt_kind = EVT_MOVE;
  }
}

static void handle_kbd_frame(const uint8_t *d, size_t len) {
  if (len < 8)
    return;
  macro_timer_cancel();
  uint8_t mod = d[1];
  send_kbd_out(mod, &d[2]);

  if (mod == 0 && d[2] == 0 && d[3] == 0) {
    s_evt_kind = EVT_KEYS_UP;
  } else {
    s_evt_a = mod;
    s_evt_b = d[2];
    s_evt_kind = EVT_KEY;
  }
}

static void handle_macro_frame(const uint8_t *d, size_t len) {
  if (len < 6)
    return;
  uint8_t mod = d[1];
  uint8_t keys[6] = {d[2], d[3], d[4], 0, 0, 0};
  uint8_t hold_ms = d[5];
  if (hold_ms < 10)
    hold_ms = 10;
  if (hold_ms > 200)
    hold_ms = 200;

  macro_timer_cancel();
  send_kbd_out(mod, keys);
  if (s_macro_timer) {
    /* Release asynchronously so the WebSocket RX loop never stalls on hold_ms.
     */
    esp_timer_start_once(s_macro_timer, (uint64_t)hold_ms * 1000ULL);
  } else {
    vTaskDelay(pdMS_TO_TICKS(hold_ms));
    send_kbd_out(0, NULL);
  }

  s_evt_a = mod;
  s_evt_b = d[2];
  s_evt_kind = EVT_MACRO;
}

static bool active_link_ready(void) {
  return (s_output == SM_OUT_USB) ? usb_hid_mounted() : port_hid_connected();
}

static void send_status_json(esp_websocket_client_handle_t ws) {
  if (!ws || !esp_websocket_client_is_connected(ws))
    return;
  char buf[192];
  bool usb = (s_output == SM_OUT_USB);
  bool ready = active_link_ready();
  const char *peer =
      usb ? (ready ? "USB HID" : "USB waiting") : port_hid_peer();
  int batt = port_battery_percent();
  int n = snprintf(buf, sizeof(buf),
                   "{\"type\":\"status\",\"bleConnected\":%s,\"output\":\"%s\","
                   "\"peer\":\"%s\",\"battery\":%d}",
                   ready ? "true" : "false", usb ? "usb" : "ble",
                   peer ? peer : "", batt);
  if (n > 0 && n < (int)sizeof(buf)) {
    esp_websocket_client_send_text(ws, buf, n, pdMS_TO_TICKS(200));
  }
}

static void ws_event_handler(void *arg, esp_event_base_t base, int32_t event_id,
                             void *event_data) {
  (void)arg;
  (void)base;
  esp_websocket_event_data_t *ev = (esp_websocket_event_data_t *)event_data;

  switch (event_id) {
  case WEBSOCKET_EVENT_CONNECTED:
    ESP_LOGI(TAG, "WebSocket connected");
    s_state = SM_CONNECTED;
    set_last_evt("connected");
    send_status_json(s_ws);
    break;

  case WEBSOCKET_EVENT_DISCONNECTED:
  case WEBSOCKET_EVENT_ERROR: {
    int sock_err = ev ? ev->error_handle.esp_transport_sock_errno : 0;
    int http_code = ev ? ev->error_handle.esp_ws_handshake_status_code : 0;
    int etype = ev ? (int)ev->error_handle.error_type : -1;
    ESP_LOGW(TAG, "WS disc/err type=%d sock_errno=%d http=%d tls=%s", etype,
             sock_err, http_code,
             ev ? esp_err_to_name(ev->error_handle.esp_tls_last_esp_err) : "?");
    port_sm_release_all();
    if (!s_stop_req && !s_restart_req && s_state != SM_WIFI_CONNECTING)
      s_state = SM_WS_CONNECTING;
    char msg[32];
    if (http_code > 0)
      snprintf(msg, sizeof(msg), "ws http %d", http_code);
    else if (sock_err > 0)
      snprintf(msg, sizeof(msg), "tcp err %d", sock_err);
    else
      snprintf(msg, sizeof(msg), "ws err %d", etype);
    set_last_evt(msg);
    break;
  }

  case WEBSOCKET_EVENT_DATA:
    if (!ev || !ev->data_ptr || ev->data_len <= 0)
      break;
    if (ev->op_code == 0x02) { /* Binary HID frame */
      const uint8_t *d = (const uint8_t *)ev->data_ptr;
      size_t len = (size_t)ev->data_len;
      s_rx_count++;
      switch (d[0]) {
      case 0x01:
        handle_mouse_frame(d, len);
        break;
      case 0x02:
        handle_kbd_frame(d, len);
        break;
      case 0x03:
        handle_macro_frame(d, len);
        break;
      case 0x04:
        port_sm_release_all();
        set_last_evt("release all");
        break;
      default:
        break;
      }
    } else if (ev->op_code == 0x01) { /* Text control frame */
      if (ev->data_len < 120) {
        char txt[128];
        memcpy(txt, ev->data_ptr, (size_t)ev->data_len);
        txt[ev->data_len] = '\0';
        if (strstr(txt, "\"ping\"")) {
          /* Echo ping payload back with type=pong for RTT measurement */
          char *p = strstr(txt, "\"ping\"");
          if (p) {
            memcpy(p, "\"pong\"", 6);
            esp_websocket_client_send_text(s_ws, txt, (int)strlen(txt),
                                           pdMS_TO_TICKS(100));
          }
        }
      }
    }
    break;

  default:
    break;
  }
}

static void stop_ws_client(void) {
  if (!s_ws)
    return;
  esp_websocket_client_stop(s_ws);
  esp_websocket_client_destroy(s_ws);
  s_ws = NULL;
}

static esp_err_t start_ws_client(const char *url) {
  stop_ws_client();

  esp_websocket_client_config_t wcfg = {
      .uri = url,
      .task_prio = 7,
      .task_stack = 3072,
      .reconnect_timeout_ms = 2500,
      .network_timeout_ms = 5000,
      .ping_interval_sec = 5,
      .pingpong_timeout_sec = 10,
      .buffer_size = 512,
  };
  if (strncmp(url, "wss://", 6) == 0) {
    wcfg.crt_bundle_attach = esp_crt_bundle_attach;
  }

  s_ws = esp_websocket_client_init(&wcfg);
  if (!s_ws) {
    ESP_LOGE(TAG, "esp_websocket_client_init failed");
    set_last_evt("ws init err");
    return ESP_ERR_NO_MEM;
  }
  esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event_handler,
                                NULL);
  esp_err_t ws_err = esp_websocket_client_start(s_ws);
  if (ws_err != ESP_OK) {
    ESP_LOGE(TAG, "esp_websocket_client_start failed: %s",
             esp_err_to_name(ws_err));
    port_heap_report("sm_ws_fail");
    char ebuf[32];
    snprintf(ebuf, sizeof(ebuf), "ws: %s", esp_err_to_name(ws_err));
    set_last_evt(ebuf);
    stop_ws_client();
    return ws_err;
  }
  return ESP_OK;
}

static void sm_worker_task(void *arg) {
  (void)arg;

  do {
    s_restart_req = false;

    char raw_url[SM_URL_MAX];
    char url[SM_URL_MAX];
    port_sm_url_get(raw_url, sizeof(raw_url));
    if (!raw_url[0]) {
      s_state = SM_NO_URL;
      break;
    }
    expand_ws_url(raw_url, url, sizeof(url));
    if (!badge_creds_wifi_any()) {
      s_state = SM_NO_WIFI;
      break;
    }

    s_state = SM_WIFI_CONNECTING;
    set_last_evt("waiting wifi lock");
    port_heap_report("sm_pre");

    if (!badge_wifi_take(8000)) {
      ESP_LOGW(TAG, "Wi-Fi busy — cannot start Super Mouse");
      set_last_evt("wifi busy");
      s_state = SM_ERR;
      break;
    }

    esp_netif_t *nif = badge_wifi_netif_once();
    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t we = esp_wifi_init(&ic);
    if (we != ESP_OK) {
      ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(we));
      char ebuf[32];
      snprintf(ebuf, sizeof(ebuf), "init: %s", esp_err_to_name(we));
      set_last_evt(ebuf);
      s_state = SM_ERR;
      badge_wifi_give();
      break;
    }

    esp_wifi_set_mode(WIFI_MODE_STA);
    we = esp_wifi_start();
    if (we != ESP_OK) {
      ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(we));
      char ebuf[32];
      snprintf(ebuf, sizeof(ebuf), "start: %s", esp_err_to_name(we));
      set_last_evt(ebuf);
      esp_wifi_deinit();
      s_state = SM_ERR;
      badge_wifi_give();
      break;
    }
    /* Required by ESP-IDF when Wi-Fi and BLE coexist on one antenna (must
     * follow esp_wifi_start). */
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    if (s_output == SM_OUT_USB)
      esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
    set_last_evt("scanning wifi");

    char ss[33] = "", pw[65] = "";
    if (!badge_wifi_pick(ss, sizeof(ss), pw, sizeof(pw))) {
      set_last_evt("no saved wifi");
      s_state = SM_NO_WIFI;
      esp_wifi_stop();
      esp_wifi_deinit();
      badge_wifi_give();
      break;
    }

    wifi_config_t wc = {0};
    snprintf((char *)wc.sta.ssid, sizeof(wc.sta.ssid), "%s", ss);
    snprintf((char *)wc.sta.password, sizeof(wc.sta.password), "%s", pw);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_connect();
    set_last_evt("joining wifi");
    ESP_LOGI(TAG, "joining Wi-Fi SSID %s", ss);

    esp_netif_ip_info_t ip = {0};
    bool addressed = false;
    int tries_left = 2, down_for = 0;
    for (int i = 0; i < 30 && !s_stop_req && !s_restart_req; i++) {
      vTaskDelay(pdMS_TO_TICKS(500));
      if (s_output == SM_OUT_USB)
        usb_hid_poll_vbus(port_battery_plugged());
      if (nif && esp_netif_get_ip_info(nif, &ip) == ESP_OK && ip.ip.addr) {
        addressed = true;
        break;
      }
      wifi_ap_record_t ap;
      if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        down_for = 0;
        set_last_evt("waiting dhcp");
        continue;
      }
      if (++down_for >= 6 && tries_left > 0) {
        tries_left--;
        down_for = 0;
        esp_wifi_connect();
      }
    }

    if (!addressed || s_stop_req || s_restart_req) {
      if (!s_stop_req && !s_restart_req) {
        ESP_LOGW(TAG, "Wi-Fi failed to get IP on SSID %s", ss);
        set_last_evt("wifi no ip");
        s_state = SM_ERR;
      }
      esp_wifi_disconnect();
      esp_wifi_stop();
      esp_wifi_deinit();
      badge_wifi_give();
      continue;
    }

    ESP_LOGI(TAG, "Wi-Fi IP " IPSTR " — connecting WebSocket %s",
             IP2STR(&ip.ip), url);
    port_heap_report("sm_wifi");

    s_state = SM_WS_CONNECTING;
    set_last_evt("connecting ws");

    if (!s_macro_timer) {
      const esp_timer_create_args_t ta = {
          .callback = macro_release_cb,
          .name = "sm_macro",
      };
      esp_timer_create(&ta, &s_macro_timer);
    }

    if (start_ws_client(url) != ESP_OK) {
      s_state = SM_ERR;
    } else {
      uint32_t tick = 0;
      uint32_t wifi_down_sec = 0;
      uint32_t dhcp_wait_sec = 0;
      uint32_t ws_stuck_sec = 0;
      uint32_t ws_reinit_fails = 0;
      bool prev_link = active_link_ready();
      sm_out_t prev_out = s_output;

      while (!s_stop_req && !s_restart_req) {
        vTaskDelay(pdMS_TO_TICKS(250));
        tick++;

        if (s_output == SM_OUT_USB)
          usb_hid_poll_vbus(port_battery_plugged());

        /* Monitor Wi-Fi and WebSocket health once per second so transient
         * AP drops or wedged sockets never leave the badge stuck on
         * "Connecting WS". */
        if ((tick % 4) == 0) {
          wifi_ap_record_t ap;
          bool wifi_up = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);
          esp_netif_ip_info_t cur_ip = {0};
          bool has_ip = wifi_up && nif &&
                        (esp_netif_get_ip_info(nif, &cur_ip) == ESP_OK) &&
                        cur_ip.ip.addr != 0;

          if (!wifi_up) {
            dhcp_wait_sec = 0;
            ws_stuck_sec = 0;
            if (s_state != SM_WIFI_CONNECTING) {
              ESP_LOGW(TAG, "Wi-Fi link lost — reconnecting");
              s_state = SM_WIFI_CONNECTING;
              set_last_evt("rejoining wifi");
            }
            wifi_down_sec++;
            if (wifi_down_sec >= 20) {
              ESP_LOGW(TAG, "Wi-Fi down 20s — rescanning");
              s_restart_req = true;
              break;
            }
            if ((wifi_down_sec % 4) == 1)
              esp_wifi_connect();
          } else if (!has_ip) {
            wifi_down_sec = 0;
            ws_stuck_sec = 0;
            s_state = SM_WIFI_CONNECTING;
            set_last_evt("waiting dhcp");
            if (++dhcp_wait_sec >= 15) {
              ESP_LOGW(TAG, "DHCP stalled — re-associating Wi-Fi");
              dhcp_wait_sec = 0;
              esp_wifi_disconnect();
              esp_wifi_connect();
            }
          } else {
            wifi_down_sec = 0;
            dhcp_wait_sec = 0;
            if (s_state == SM_WIFI_CONNECTING) {
              ESP_LOGI(TAG, "Wi-Fi restored (" IPSTR ") — restarting WS",
                       IP2STR(&cur_ip.ip));
              s_state = SM_WS_CONNECTING;
              set_last_evt("connecting ws");
              ws_stuck_sec = 0;
              if (start_ws_client(url) != ESP_OK) {
                s_state = SM_ERR;
                break;
              }
            } else if (s_state == SM_WS_CONNECTING) {
              if (++ws_stuck_sec >= 15) {
                ws_stuck_sec = 0;
                ws_reinit_fails++;
                if (ws_reinit_fails >= 3) {
                  ESP_LOGW(TAG, "WS unreachable 45s — cycling Wi-Fi");
                  ws_reinit_fails = 0;
                  s_state = SM_WIFI_CONNECTING;
                  set_last_evt("rejoining wifi");
                  esp_wifi_disconnect();
                  esp_wifi_connect();
                } else {
                  ESP_LOGW(TAG, "WS stuck 15s — cycling WS client");
                  set_last_evt("retrying ws");
                  if (start_ws_client(url) != ESP_OK) {
                    s_state = SM_ERR;
                    break;
                  }
                }
              }
            } else if (s_state == SM_CONNECTED) {
              ws_stuck_sec = 0;
              ws_reinit_fails = 0;
            }
          }
        }

        bool link_now = active_link_ready();
        sm_out_t out_now = s_output;
        if (link_now != prev_link || out_now != prev_out || (tick % 40) == 0) {
          prev_link = link_now;
          prev_out = out_now;
          send_status_json(s_ws);
        }
      }
    }

    stop_ws_client();
    port_sm_release_all();
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();
    badge_wifi_give();
  } while (s_restart_req && !s_stop_req);

  esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
  badge_ble_adv_pause(false);

  if (s_stop_req)
    s_state = SM_OFF;
  s_task_running = false;
  ESP_LOGI(TAG, "Super Mouse network bridge stopped");
  vTaskDelete(NULL);
}

void port_sm_start(void) {
  if (s_task_running)
    return;
  s_stop_req = false;
  s_restart_req = false;
  s_rx_count = 0;
  set_last_evt("starting");
  s_task_running = true;
  if (xTaskCreate(sm_worker_task, "supermouse", 4096, NULL, 5, NULL) !=
      pdPASS) {
    ESP_LOGE(TAG, "could not create supermouse task — out of internal RAM");
    set_last_evt("no ram (task)");
    s_task_running = false;
    s_state = SM_ERR;
  }
}

void port_sm_reconnect(void) {
  if (s_task_running) {
    s_restart_req = true;
    set_last_evt("reconnecting");
    return;
  }
  port_sm_start();
}

void port_sm_stop(void) {
  s_restart_req = false;
  s_stop_req = true;
  port_sm_release_all();
  /* Wait briefly for clean Wi-Fi teardown so launcher transitions stay orderly.
   */
  for (int i = 0; i < 60 && s_task_running; i++) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  if (!s_task_running)
    s_state = SM_OFF;
}

void port_sm_leave(void) {
  port_sm_stop();
  usb_hid_reboot_if_used();
}

sm_out_t port_sm_output_get(void) { return s_output; }

bool port_sm_output_set(sm_out_t mode) {
  if (mode == s_output)
    return true;
  if (mode == SM_OUT_USB) {
    port_hid_release_all();
    if (!usb_hid_start()) {
      const char *err = usb_hid_error();
      char msg[32];
      snprintf(msg, sizeof(msg), "usb: %s", (err && err[0]) ? err : "init err");
      set_last_evt(msg);
      return false;
    }
    s_output = SM_OUT_USB;
    badge_ble_adv_pause(true);
    esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
    set_last_evt("output: USB");
    return true;
  }
  usb_hid_disconnect();
  s_output = SM_OUT_BLE;
  esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
  badge_ble_adv_pause(false);
  set_last_evt("output: BLE");
  return true;
}

bool port_sm_usb_mounted(void) { return usb_hid_mounted(); }
bool port_sm_usb_used(void) { return usb_hid_used(); }
void port_sm_usb_error(char *out, size_t len) {
  if (out && len)
    snprintf(out, len, "%s", usb_hid_error());
}

sm_state_t port_sm_state(void) { return s_state; }
uint32_t port_sm_rx_count(void) { return s_rx_count; }

void port_sm_last_event(char *out, size_t len) {
  if (!out || !len)
    return;
  switch (s_evt_kind) {
  case EVT_MOVE:
    snprintf(out, len, "move");
    break;
  case EVT_WHEEL:
    snprintf(out, len, "wheel %+d", s_evt_a);
    break;
  case EVT_BTN:
    snprintf(out, len, "btn 0x%X", s_evt_a);
    break;
  case EVT_KEY:
    snprintf(out, len, "key m:%02X k:%02X", s_evt_a & 0xFF, s_evt_b & 0xFF);
    break;
  case EVT_KEYS_UP:
    snprintf(out, len, "keys up");
    break;
  case EVT_MACRO:
    snprintf(out, len, "macro %02X+%02X", s_evt_a & 0xFF, s_evt_b & 0xFF);
    break;
  case EVT_TEXT:
  default:
    snprintf(out, len, "%s", s_last_evt);
    break;
  }
}
