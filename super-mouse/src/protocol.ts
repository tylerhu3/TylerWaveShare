/* Shared binary HID wire protocol between Frontend, Backend, and ESP32-S3 Badge.
 *
 * Opcodes:
 *   0x01 Mouse:    [0x01, buttons_u8, dx_i16_le, dy_i16_le, wheel_i8]  (7 bytes)
 *   0x02 Keyboard: [0x02, modifiers_u8, k0, k1, k2, k3, k4, k5]        (8 bytes)
 *   0x03 MacroTap: [0x03, modifiers_u8, k0, k1, k2, hold_ms_u8]        (6 bytes)
 *   0x04 Release:  [0x04]                                              (1 byte)
 */

export const OP_MOUSE   = 0x01;
export const OP_KBD     = 0x02;
export const OP_MACRO   = 0x03;
export const OP_RELEASE = 0x04;

export const RELEASE_ALL_FRAME = Buffer.from([OP_RELEASE]);

export function isValidHidFrame(buf: Buffer): boolean {
  if (buf.length < 1) return false;
  switch (buf[0]) {
    case OP_MOUSE:   return buf.length >= 7;
    case OP_KBD:     return buf.length >= 8;
    case OP_MACRO:   return buf.length >= 6;
    case OP_RELEASE: return buf.length >= 1;
    default:         return false;
  }
}
