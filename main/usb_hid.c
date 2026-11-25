/* USB HID composite Mouse + 6KRO Keyboard for Super Mouse (ESP32-S3 only). */
#include "usb_hid.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_private/usb_phy.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tusb.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "usb_hid";

static usb_phy_handle_t s_phy;
static TaskHandle_t s_task;
static volatile bool s_inited;        /* TinyUSB + PHY owned by USB HID */
static volatile bool s_want_attached; /* Super Mouse output mode is USB */
static volatile bool s_attached;      /* D+ pull-up currently enabled */
static volatile bool s_suspended;     /* bus suspended by host or unplugged */
static volatile bool s_remote_wakeup_en;
static volatile int s_init_res; /* 0=pending, 1=ok, -1=failed */
static uint8_t s_vbus_off_ticks;
static uint8_t s_vbus_on_ticks;
static char s_err[48];

static portMUX_TYPE s_hid_mux = portMUX_INITIALIZER_UNLOCKED;
static StaticSemaphore_t s_ready_sem_buf;
static SemaphoreHandle_t s_ready_sem;
static int32_t s_pend_dx;
static int32_t s_pend_dy;
static int32_t s_pend_wheel;
static uint8_t s_pend_buttons;
static bool s_pend_valid;
static uint8_t s_last_buttons;

enum {
  RID_MOUSE = 1,
  RID_KBD = 2,
};

/* USB HID composite report layout:
 *   ID 1 = mouse, 6 bytes    [buttons][dx_i16_le][dy_i16_le][wheel_i8]
 *   ID 2 = keyboard, 8 bytes [modifiers][0][k0..k5]
 * 16-bit relative X/Y avoids chunking large deltas into multiple USB frames. */
static const uint8_t s_hid_report_desc[] = {
    0x05,
    0x01, /* Usage Page (Generic Desktop) */
    0x09,
    0x02, /*   Usage (Mouse) */
    0xA1,
    0x01, /*   Collection (Application) */
    0x85,
    RID_MOUSE, /*     Report ID (1) */
    0x09,
    0x01, /*     Usage (Pointer) */
    0xA1,
    0x00, /*     Collection (Physical) */
    0x05,
    0x09, /*       Usage Page (Buttons) */
    0x19,
    0x01, /*       Usage Minimum (1) */
    0x29,
    0x03, /*       Usage Maximum (3) */
    0x15,
    0x00,
    0x25,
    0x01,
    0x95,
    0x03,
    0x75,
    0x01,
    0x81,
    0x02, /*       Input (Data, Variable, Absolute) — 3 buttons */
    0x95,
    0x01,
    0x75,
    0x05,
    0x81,
    0x03, /*       Input (Constant) — 5 pad bits */
    0x05,
    0x01, /*       Usage Page (Generic Desktop) */
    0x09,
    0x30, /*       Usage (X) */
    0x09,
    0x31, /*       Usage (Y) */
    0x16,
    0x01,
    0x80, /*       Logical Minimum (-32767) */
    0x26,
    0xFF,
    0x7F, /*       Logical Maximum (32767) */
    0x75,
    0x10, /*       Report Size (16) */
    0x95,
    0x02, /*       Report Count (2) */
    0x81,
    0x06, /*       Input (Data, Variable, Relative) */
    0x09,
    0x38, /*       Usage (Wheel) */
    0x15,
    0x81, /*       Logical Minimum (-127) */
    0x25,
    0x7F, /*       Logical Maximum (127) */
    0x75,
    0x08, /*       Report Size (8) */
    0x95,
    0x01, /*       Report Count (1) */
    0x81,
    0x06, /*       Input (Data, Variable, Relative) */
    0xC0,
    0xC0,

    /* Keyboard (6KRO) */
    0x05,
    0x01, /* Usage Page (Generic Desktop) */
    0x09,
    0x06, /*   Usage (Keyboard) */
    0xA1,
    0x01, /*   Collection (Application) */
    0x85,
    RID_KBD, /*     Report ID (2) */
    0x05,
    0x07, /*     Usage Page (Keyboard) */
    0x19,
    0xE0,
    0x29,
    0xE7,
    0x15,
    0x00,
    0x25,
    0x01,
    0x75,
    0x01,
    0x95,
    0x08,
    0x81,
    0x02, /*     Input — 8 modifier bits */
    0x95,
    0x01,
    0x75,
    0x08,
    0x81,
    0x03, /*     Input (Constant) — reserved byte */
    0x95,
    0x06,
    0x75,
    0x08,
    0x15,
    0x00,
    0x25,
    0x65,
    0x05,
    0x07,
    0x19,
    0x00,
    0x29,
    0x65,
    0x81,
    0x00, /*     Input — 6 simultaneous keys */
    0xC0,
};

/* Espressif VID (0x303A) with a distinct PID from USB MSC (0x4002) so hosts
 * do not cache MSC descriptors for the HID device. */
#define USB_HID_VID 0x303A
#define USB_HID_PID 0x4004

static const tusb_desc_device_t s_desc_dev = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_HID_VID,
    .idProduct = USB_HID_PID,
    .bcdDevice = 0x0101,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

enum { ITF_NUM_HID = 0, ITF_NUM_HID_TOTAL };
#define EPNUM_HID_IN 0x81
#define CFG_HID_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)

static const uint8_t s_desc_cfg[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_HID_TOTAL, 0, CFG_HID_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, 4, HID_ITF_PROTOCOL_NONE,
                       sizeof(s_hid_report_desc), EPNUM_HID_IN,
                       CFG_TUD_HID_EP_BUFSIZE, 1),
};

static const char *const s_desc_str[] = {
    (const char[]){0x09, 0x04}, /* 0: English (US) */
    "Badge",                    /* 1: manufacturer */
    "Badge Super Mouse",        /* 2: product */
    "BADGEHID1",                /* 3: serial */
    "Super Mouse HID",          /* 4: HID interface */
};

const uint8_t *usb_hid_desc_device(void) {
  return (const uint8_t *)&s_desc_dev;
}
const uint8_t *usb_hid_desc_config(void) { return s_desc_cfg; }

const uint16_t *usb_hid_desc_string(uint8_t index) {
  static uint16_t buf[32];
  uint8_t n;
  if (index == 0) {
    memcpy(&buf[1], s_desc_str[0], 2);
    n = 1;
  } else {
    if (index >= sizeof(s_desc_str) / sizeof(s_desc_str[0]))
      return NULL;
    const char *s = s_desc_str[index];
    n = (uint8_t)strlen(s);
    if (n > 31)
      n = 31;
    for (uint8_t i = 0; i < n; i++)
      buf[1 + i] = s[i];
  }
  buf[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * n + 2));
  return buf;
}

static void reset_pending_state(void) {
  portENTER_CRITICAL(&s_hid_mux);
  s_pend_dx = 0;
  s_pend_dy = 0;
  s_pend_wheel = 0;
  s_pend_buttons = 0;
  s_pend_valid = false;
  s_last_buttons = 0;
  portEXIT_CRITICAL(&s_hid_mux);
}

/* ── TinyUSB device lifecycle callbacks ────────────────────── */
void tud_mount_cb(void) {
  if (!s_inited)
    return;
  s_suspended = false;
  s_remote_wakeup_en = false;
  reset_pending_state();
  if (s_ready_sem)
    xSemaphoreGive(s_ready_sem);
  ESP_LOGI(TAG, "USB HID mounted");
}

void tud_umount_cb(void) {
  if (!s_inited)
    return;
  s_suspended = false;
  s_remote_wakeup_en = false;
  reset_pending_state();
  ESP_LOGI(TAG, "USB HID unmounted");
}

void tud_suspend_cb(bool remote_wakeup_en) {
  if (!s_inited)
    return;
  s_suspended = true;
  s_remote_wakeup_en = remote_wakeup_en;
  reset_pending_state();
  ESP_LOGI(TAG, "USB HID suspended (rwu=%d)", remote_wakeup_en);
}

void tud_resume_cb(void) {
  if (!s_inited)
    return;
  s_suspended = false;
  ESP_LOGI(TAG, "USB HID resumed");
}

/* ── TinyUSB HID callbacks ─────────────────────────────────── */
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
  (void)instance;
  return s_hid_report_desc;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen) {
  (void)instance;
  (void)report_id;
  (void)report_type;
  (void)buffer;
  (void)reqlen;
  return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type, uint8_t const *buffer,
                           uint16_t bufsize) {
  (void)instance;
  (void)report_id;
  (void)report_type;
  (void)buffer;
  (void)bufsize;
}

static int16_t clamp16(int32_t v) {
  if (v > 32767)
    return 32767;
  if (v < -32767)
    return -32767;
  return (int16_t)v;
}

static int8_t clamp8(int32_t v) {
  if (v > 127)
    return 127;
  if (v < -127)
    return -127;
  return (int8_t)v;
}

static void send_mouse_report_now(uint8_t buttons, int16_t dx, int16_t dy,
                                  int8_t wheel) {
  uint8_t r[6] = {
      (uint8_t)(buttons & 0x07),
      (uint8_t)((uint16_t)dx & 0xFF),
      (uint8_t)(((uint16_t)dx >> 8) & 0xFF),
      (uint8_t)((uint16_t)dy & 0xFF),
      (uint8_t)(((uint16_t)dy >> 8) & 0xFF),
      (uint8_t)wheel,
  };
  s_last_buttons = buttons & 0x07;
  tud_hid_report(RID_MOUSE, r, sizeof(r));
}

void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report,
                                uint16_t len) {
  (void)instance;
  (void)report;
  (void)len;
  bool flush = false;
  uint8_t buttons = 0;
  int16_t dx = 0, dy = 0;
  int8_t wh = 0;
  portENTER_CRITICAL(&s_hid_mux);
  if (s_pend_valid) {
    flush = true;
    buttons = s_pend_buttons;
    dx = clamp16(s_pend_dx);
    dy = clamp16(s_pend_dy);
    wh = clamp8(s_pend_wheel);
    s_pend_dx = 0;
    s_pend_dy = 0;
    s_pend_wheel = 0;
    s_pend_valid = false;
  }
  portEXIT_CRITICAL(&s_hid_mux);
  if (flush) {
    send_mouse_report_now(buttons, dx, dy, wh);
  } else if (s_ready_sem) {
    xSemaphoreGive(s_ready_sem);
  }
}

/* ── task & lifecycle ──────────────────────────────────────── */
static void usb_hid_task(void *arg) {
  (void)arg;
  /* tusb_init() binds the USB interrupt to the calling core (core 0). */
  if (!tusb_init()) {
    ESP_LOGE(TAG, "tusb_init failed");
    snprintf(s_err, sizeof(s_err), "tusb_init failed");
    s_inited = false;
    s_attached = false;
    s_init_res = -1;
    /* Suspend so usb_hid_start() can safely clean up via vTaskDeleteWithCaps().
     */
    vTaskSuspend(NULL);
    return;
  }
  s_init_res = 1;
  ESP_LOGI(TAG, "USB HID started");
  while (s_inited)
    tud_task();
  vTaskSuspend(NULL);
}

bool usb_hid_start(void) {
  s_err[0] = '\0';
  if (!s_ready_sem)
    s_ready_sem = xSemaphoreCreateBinaryStatic(&s_ready_sem_buf);
  if (s_inited) {
    s_want_attached = true;
    s_vbus_off_ticks = 0;
    s_vbus_on_ticks = 0;
    if (!s_attached) {
      reset_pending_state();
      s_suspended = false;
      tud_connect();
      s_attached = true;
    }
    return true;
  }

  usb_phy_config_t cfg = {
      .controller = USB_PHY_CTRL_OTG,
      .target = USB_PHY_TARGET_INT,
      .otg_mode = USB_OTG_MODE_DEVICE,
  };
  esp_err_t pe = usb_new_phy(&cfg, &s_phy);
  if (pe != ESP_OK) {
    snprintf(s_err, sizeof(s_err), "phy: %s", esp_err_to_name(pe));
    ESP_LOGE(TAG, "could not take USB PHY: %s", esp_err_to_name(pe));
    return false;
  }

  s_inited = true;
  s_want_attached = true;
  s_attached = true;
  s_suspended = false;
  s_remote_wakeup_en = false;
  s_vbus_off_ticks = 0;
  s_vbus_on_ticks = 0;
  s_init_res = 0;

  /* Put the TinyUSB task stack in PSRAM: Wi-Fi + BLE + WebSocket leave only
   * ~9 KB of internal SRAM free, which causes a 4 KB internal stack allocation
   * to fail. usb_hid_task never touches SPI flash/NVS, so a PSRAM stack is
   * safe. Priority 8 services USB IN completions ahead of LVGL and WS tasks. */
  BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(
      usb_hid_task, "usb_hid", 4096, NULL, 8, &s_task, 0,
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (ok != pdPASS) {
    size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    snprintf(s_err, sizeof(s_err), "task oom (int %uB)", (unsigned)free_int);
    ESP_LOGE(TAG, "usb_hid task create failed (int free=%u)",
             (unsigned)free_int);
    s_inited = false;
    s_want_attached = false;
    s_attached = false;
    usb_del_phy(s_phy);
    s_phy = NULL;
    return false;
  }

  for (int i = 0; i < 50 && s_init_res == 0; i++) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (s_init_res != 1) {
    if (s_init_res == 0)
      snprintf(s_err, sizeof(s_err), "tusb_init timeout");
    s_inited = false;
    s_want_attached = false;
    s_attached = false;
    if (s_task) {
      vTaskDeleteWithCaps(s_task);
      s_task = NULL;
    }
    if (s_phy) {
      usb_del_phy(s_phy);
      s_phy = NULL;
    }
    return false;
  }
  return true;
}

const char *usb_hid_error(void) { return s_err; }

void usb_hid_disconnect(void) {
  s_want_attached = false;
  s_vbus_off_ticks = 0;
  s_vbus_on_ticks = 0;
  if (!s_inited || !s_attached)
    return;
  usb_hid_release_all();
  vTaskDelay(pdMS_TO_TICKS(15));
  tud_disconnect();
  s_attached = false;
  s_suspended = false;
}

/* ESP32-S3 internal PHY forces BVALID high (no VBUS sense GPIO), so unplugging
 * the cable only triggers USB suspend rather than a disconnect. Polling the
 * AXP2101 VBUS bit detaches cleanly on unplug and re-attaches after a short
 * debounce when plugged into a new host so re-enumeration always succeeds. */
void usb_hid_poll_vbus(bool vbus) {
  if (!s_inited || !s_want_attached) {
    s_vbus_off_ticks = 0;
    s_vbus_on_ticks = 0;
    return;
  }
  if (!vbus) {
    s_vbus_on_ticks = 0;
    if (s_attached && ++s_vbus_off_ticks >= 2) {
      ESP_LOGI(TAG, "VBUS lost — detaching USB HID");
      reset_pending_state();
      tud_disconnect();
      s_attached = false;
      s_suspended = false;
    }
  } else {
    s_vbus_off_ticks = 0;
    if (!s_attached && ++s_vbus_on_ticks >= 2) {
      ESP_LOGI(TAG, "VBUS detected — attaching USB HID");
      reset_pending_state();
      s_suspended = false;
      tud_connect();
      s_attached = true;
    }
  }
}

void usb_hid_reboot_if_used(void) {
  if (!s_inited)
    return;
  usb_hid_release_all();
  vTaskDelay(pdMS_TO_TICKS(20));
  tud_disconnect();
  vTaskDelay(pdMS_TO_TICKS(150));
  esp_restart();
}

bool usb_hid_active(void) { return s_inited; }
bool usb_hid_used(void) { return s_inited; }
bool usb_hid_mounted(void) { return s_inited && s_attached && tud_mounted(); }

/* Wait for the interrupt IN endpoint to clear via completion semaphore so
 * button changes and keyboard reports wake immediately without 1 ms polling. */
static bool wait_ready(void) {
  if (!s_inited || !s_attached || !tud_mounted())
    return false;
  if (s_suspended || tud_suspended()) {
    if (s_remote_wakeup_en)
      tud_remote_wakeup();
    return false;
  }
  if (tud_hid_ready())
    return true;
  if (s_ready_sem) {
    /* Drain stale token before waiting for the next completion. */
    xSemaphoreTake(s_ready_sem, 0);
    if (tud_hid_ready())
      return true;
    xSemaphoreTake(s_ready_sem, pdMS_TO_TICKS(15));
    return tud_hid_ready();
  }
  for (int i = 0; i < 15; i++) {
    if (tud_hid_ready())
      return true;
    vTaskDelay(1);
  }
  return tud_hid_ready();
}

void usb_hid_mouse(int dx, int dy, unsigned buttons, int wheel) {
  if (!s_inited || !s_attached || !tud_mounted())
    return;
  if (s_suspended || tud_suspended()) {
    if (s_remote_wakeup_en)
      tud_remote_wakeup();
    return;
  }

  uint8_t btn = (uint8_t)(buttons & 0x07);

  /* Non-blocking coalescing for pure motion/wheel when IN endpoint is busy:
   * tud_hid_report_complete_cb() flushes the merged delta on the next 1 ms
   * frame so the WebSocket RX task never stalls. */
  if (!tud_hid_ready() && btn == s_last_buttons) {
    portENTER_CRITICAL(&s_hid_mux);
    s_pend_dx += dx;
    s_pend_dy += dy;
    s_pend_wheel += wheel;
    s_pend_buttons = btn;
    s_pend_valid = true;
    portEXIT_CRITICAL(&s_hid_mux);
    /* Re-check in case completion fired just before we set s_pend_valid. */
    if (!tud_hid_ready())
      return;
    dx = 0;
    dy = 0;
    wheel = 0;
  }

  if (!wait_ready())
    return;

  int32_t total_dx = dx;
  int32_t total_dy = dy;
  int32_t total_wh = wheel;
  portENTER_CRITICAL(&s_hid_mux);
  if (s_pend_valid) {
    total_dx += s_pend_dx;
    total_dy += s_pend_dy;
    total_wh += s_pend_wheel;
    s_pend_dx = 0;
    s_pend_dy = 0;
    s_pend_wheel = 0;
    s_pend_valid = false;
  }
  portEXIT_CRITICAL(&s_hid_mux);

  if (total_dx == 0 && total_dy == 0 && total_wh == 0 && btn == s_last_buttons)
    return;

  send_mouse_report_now(btn, clamp16(total_dx), clamp16(total_dy),
                        clamp8(total_wh));
}

void usb_hid_keyboard_report(uint8_t modifiers, const uint8_t keys[6]) {
  if (!wait_ready())
    return;
  uint8_t r[8] = {modifiers, 0, 0, 0, 0, 0, 0, 0};
  if (keys)
    memcpy(&r[2], keys, 6);
  tud_hid_report(RID_KBD, r, sizeof(r));
}

void usb_hid_release_all(void) {
  if (!s_inited || !s_attached || !tud_mounted())
    return;
  portENTER_CRITICAL(&s_hid_mux);
  s_pend_dx = 0;
  s_pend_dy = 0;
  s_pend_wheel = 0;
  s_pend_valid = false;
  portEXIT_CRITICAL(&s_hid_mux);
  usb_hid_mouse(0, 0, 0, 0);
  usb_hid_keyboard_report(0, NULL);
}
