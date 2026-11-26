/* Super Mouse USB-serial source (ESP32-S3 firmware only).
 *
 * The computer on the USB cable runs the Super Mouse web app, which opens the
 * badge's USB-Serial/JTAG port with WebSerial and writes HID frames. This task
 * polls the USJ RX FIFO and forwards each frame to the BLE HID host. Wi-Fi
 * stays off, so there is no Wi-Fi/BLE coexistence time-slicing.
 *
 * Serial frame (host -> badge), every frame starts with sync byte 0xA5:
 *   0x01 Mouse:     [0xA5, 0x01, buttons_u8, dx_i16_le, dy_i16_le, wheel_i8]
 *   0x02 Keyboard:  [0xA5, 0x02, modifiers_u8, k0, k1, k2, k3, k4, k5]
 *   0x03 KeyTap:    [0xA5, 0x03, modifiers_u8, k0, k1, k2, hold_ms_u8]
 *   0x04 Release:   [0xA5, 0x04]
 *   0x05 Heartbeat: [0xA5, 0x05]
 *
 * Status (badge -> host) is a text line on the console, which shares the USJ
 * port with ESP_LOG output; the web app ignores every other line:
 *   @SM {"live":true,"ble":true,"peer":"...","batt":87,"rx":123}
 *
 * The USJ is the secondary console (output only), so nothing else reads its
 * RX FIFO. Reading the FIFO directly through the LL layer avoids installing
 * the USJ driver, which would make console writes block when no host reads. */
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/usb_serial_jtag_ll.h"
#include "port.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "super_mouse";

#define SM_SYNC 0xA5
#define OP_MOUSE 0x01
#define OP_KBD 0x02
#define OP_MACRO 0x03
#define OP_RELEASE 0x04
#define OP_HEARTBEAT 0x05

#define LIVE_TIMEOUT_MS 3000  /* web app sends a heartbeat every 1 s */
#define PARTIAL_TIMEOUT_MS 100
#define STATUS_PERIOD_MS 1000

static volatile sm_state_t s_state = SM_OFF;
static volatile bool s_stop_req;
static volatile bool s_task_running;
static volatile uint32_t s_rx_count;
static char s_last_evt[32] = "idle";
static esp_timer_handle_t s_macro_timer;

/* Hot-path event tag so frames never call snprintf on the RX task; formatted
 * lazily in port_sm_last_event() at the UI poll rate. */
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

/* RX parser state (only touched by the worker task). */
static uint8_t s_frame[8];
static uint8_t s_pos;
static uint8_t s_need;
static bool s_synced;

static void macro_release_cb(void *arg) {
  (void)arg;
  port_hid_keyboard_report(0, NULL);
}

static void macro_timer_cancel(void) {
  if (s_macro_timer)
    esp_timer_stop(s_macro_timer);
}

void port_sm_release_all(void) {
  macro_timer_cancel();
  port_hid_release_all();
}

static void set_last_evt(const char *msg) {
  snprintf(s_last_evt, sizeof(s_last_evt), "%s", msg);
  s_evt_kind = EVT_TEXT;
}

static void handle_mouse_frame(const uint8_t *d) {
  uint8_t buttons = d[1] & 0x07;
  int16_t dx = (int16_t)((uint16_t)d[2] | ((uint16_t)d[3] << 8));
  int16_t dy = (int16_t)((uint16_t)d[4] | ((uint16_t)d[5] << 8));
  int8_t wh = (int8_t)d[6];

  /* BLE HID report map uses 8-bit X/Y; chunk large deltas into [-127, 127]. */
  bool first = true;
  do {
    int sx = dx > 127 ? 127 : (dx < -127 ? -127 : dx);
    int sy = dy > 127 ? 127 : (dy < -127 ? -127 : dy);
    port_hid_mouse(sx, sy, buttons, first ? (int)wh : 0);
    dx -= (int16_t)sx;
    dy -= (int16_t)sy;
    first = false;
  } while (dx != 0 || dy != 0);

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

static void handle_kbd_frame(const uint8_t *d) {
  macro_timer_cancel();
  port_hid_keyboard_report(d[1], &d[2]);
  if (d[1] == 0 && d[2] == 0 && d[3] == 0) {
    s_evt_kind = EVT_KEYS_UP;
  } else {
    s_evt_a = d[1];
    s_evt_b = d[2];
    s_evt_kind = EVT_KEY;
  }
}

static void handle_macro_frame(const uint8_t *d) {
  uint8_t keys[6] = {d[2], d[3], d[4], 0, 0, 0};
  uint8_t hold_ms = d[5];
  if (hold_ms < 10)
    hold_ms = 10;
  if (hold_ms > 200)
    hold_ms = 200;
  macro_timer_cancel();
  port_hid_keyboard_report(d[1], keys);
  /* Release from esp_timer so the RX loop never stalls for hold_ms. */
  if (!s_macro_timer || esp_timer_start_once(s_macro_timer,
                                             (uint64_t)hold_ms * 1000ULL) !=
                            ESP_OK) {
    vTaskDelay(pdMS_TO_TICKS(hold_ms));
    port_hid_keyboard_report(0, NULL);
  }
  s_evt_a = d[1];
  s_evt_b = d[2];
  s_evt_kind = EVT_MACRO;
}

static uint8_t frame_len(uint8_t op) {
  switch (op) {
  case OP_MOUSE:
    return 7;
  case OP_KBD:
    return 8;
  case OP_MACRO:
    return 6;
  case OP_RELEASE:
  case OP_HEARTBEAT:
    return 1;
  default:
    return 0;
  }
}

static void dispatch(const uint8_t *d) {
  if (d[0] != OP_HEARTBEAT)
    s_rx_count++;
  switch (d[0]) {
  case OP_MOUSE:
    handle_mouse_frame(d);
    break;
  case OP_KBD:
    handle_kbd_frame(d);
    break;
  case OP_MACRO:
    handle_macro_frame(d);
    break;
  case OP_RELEASE:
    port_sm_release_all();
    set_last_evt("release all");
    break;
  default: /* heartbeat: only refreshes liveness */
    break;
  }
}

/* Returns true when a complete frame was dispatched. */
static bool feed(uint8_t b) {
  if (!s_synced) {
    s_synced = (b == SM_SYNC);
    s_pos = 0;
    return false;
  }
  if (s_pos == 0) {
    s_need = frame_len(b);
    if (!s_need) { /* bad opcode: resync, the byte may itself be a sync */
      s_synced = (b == SM_SYNC);
      return false;
    }
  }
  s_frame[s_pos++] = b;
  if (s_pos < s_need)
    return false;
  dispatch(s_frame);
  s_synced = false;
  s_pos = 0;
  return true;
}

static void parser_reset(void) {
  s_synced = false;
  s_pos = 0;
}

static void drain_rx(void) {
  uint8_t junk[64];
  while (usb_serial_jtag_ll_rxfifo_data_available())
    usb_serial_jtag_ll_read_rxfifo(junk, sizeof(junk));
}

static void print_status(bool live, bool ble) {
  const char *peer = ble ? port_hid_peer() : "";
  char safe[32];
  size_t n = 0;
  /* Keep the JSON valid whatever the host's name contains. */
  for (const char *p = peer ? peer : ""; *p && n < sizeof(safe) - 1; p++)
    safe[n++] = (*p == '"' || *p == '\\' || (unsigned char)*p < 0x20) ? '_' : *p;
  safe[n] = '\0';
  printf("@SM {\"live\":%s,\"ble\":%s,\"peer\":\"%s\",\"batt\":%d,\"rx\":%lu}\n",
         live ? "true" : "false", ble ? "true" : "false", safe,
         port_battery_percent(), (unsigned long)s_rx_count);
}

static void sm_worker_task(void *arg) {
  (void)arg;
  if (!s_macro_timer) {
    const esp_timer_create_args_t ta = {
        .callback = macro_release_cb,
        .name = "sm_macro",
    };
    esp_timer_create(&ta, &s_macro_timer);
  }

  /* Bytes written while the app was closed are stale input. */
  drain_rx();
  parser_reset();
  s_state = SM_WAITING;
  set_last_evt("waiting for web app");

  int64_t now = esp_timer_get_time() / 1000;
  int64_t last_byte = now;
  int64_t last_frame = 0;
  int64_t last_status = 0;
  bool live = false;
  bool prev_ble = port_hid_connected();

  while (!s_stop_req) {
    uint8_t buf[64];
    bool got = false;
    while (usb_serial_jtag_ll_rxfifo_data_available()) {
      uint32_t n = usb_serial_jtag_ll_read_rxfifo(buf, sizeof(buf));
      if (!n)
        break;
      got = true;
      for (uint32_t i = 0; i < n; i++) {
        if (feed(buf[i]))
          last_frame = esp_timer_get_time() / 1000;
      }
    }
    now = esp_timer_get_time() / 1000;
    if (got)
      last_byte = now;
    else if (s_synced && now - last_byte > PARTIAL_TIMEOUT_MS)
      parser_reset();

    bool live_now = last_frame && (now - last_frame) < LIVE_TIMEOUT_MS;
    if (live_now != live) {
      live = live_now;
      if (live) {
        s_state = SM_LIVE;
        set_last_evt("web app live");
        ESP_LOGI(TAG, "web app connected over USB");
      } else {
        /* Page closed or cable pulled: never leave keys or buttons held. */
        port_sm_release_all();
        s_state = SM_WAITING;
        set_last_evt("web app gone");
        ESP_LOGI(TAG, "web app silent, released all");
      }
      last_status = 0;
    }

    bool ble = port_hid_connected();
    if (ble != prev_ble) {
      prev_ble = ble;
      last_status = 0;
    }
    if (now - last_status >= STATUS_PERIOD_MS) {
      last_status = now;
      print_status(live, ble);
    }

    if (!got)
      vTaskDelay(pdMS_TO_TICKS(2));
  }

  port_sm_release_all();
  s_state = SM_OFF;
  s_task_running = false;
  ESP_LOGI(TAG, "Super Mouse USB source stopped");
  vTaskDelete(NULL);
}

void port_sm_start(void) {
  if (s_task_running)
    return;
  s_stop_req = false;
  s_rx_count = 0;
  set_last_evt("starting");
  s_task_running = true;
  if (xTaskCreate(sm_worker_task, "supermouse", 4096, NULL, 6, NULL) !=
      pdPASS) {
    ESP_LOGE(TAG, "could not create supermouse task — out of internal RAM");
    set_last_evt("no ram (task)");
    s_task_running = false;
    s_state = SM_ERR;
  }
}

void port_sm_stop(void) {
  s_stop_req = true;
  for (int i = 0; i < 40 && s_task_running; i++)
    vTaskDelay(pdMS_TO_TICKS(10));
  port_sm_release_all();
  if (!s_task_running)
    s_state = SM_OFF;
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
