/* USB HID mouse + 6KRO keyboard mode for Super Mouse (ESP32-S3 only).
 *
 * Shares the internal USB PHY with USB-Serial/JTAG and USB MSC. Starting USB
 * HID hands the PHY to USB-OTG (serial port vanishes). Toggling back to BLE
 * detaches via tud_disconnect() in place; leaving the app reboots to restore
 * USB-Serial/JTAG, matching usb_msc.c. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

bool usb_hid_start(void);       /* bring up (or re-attach) USB HID */
void usb_hid_disconnect(void);  /* detach from host without rebooting */
void usb_hid_poll_vbus(bool vbus); /* detach on VBUS loss, re-attach on return */
void usb_hid_reboot_if_used(void); /* restore USB-Serial/JTAG via restart */

bool usb_hid_active(void);      /* true while USB HID owns the USB descriptors */
bool usb_hid_used(void);        /* true if USB OTG was started this boot */
bool usb_hid_mounted(void);     /* true when attached & enumerated by a host */
const char *usb_hid_error(void); /* last startup failure reason, or "" */

void usb_hid_mouse(int dx, int dy, unsigned buttons, int wheel);
void usb_hid_keyboard_report(uint8_t modifiers, const uint8_t keys[6]);
void usb_hid_release_all(void);

/* Descriptor helpers used by usb_msc.c's TinyUSB descriptor dispatchers. */
const uint8_t  *usb_hid_desc_device(void);
const uint8_t  *usb_hid_desc_config(void);
const uint16_t *usb_hid_desc_string(uint8_t index);
