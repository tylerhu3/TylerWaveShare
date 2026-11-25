/* USB HID Usage Page 0x07 keycodes + modifier bitmasks + DOM KeyboardEvent.code mapping. */

export const MOD_LCTRL  = 0x01;
export const MOD_LSHIFT = 0x02;
export const MOD_LALT   = 0x04;
export const MOD_LMETA  = 0x08;
export const MOD_RCTRL  = 0x10;
export const MOD_RSHIFT = 0x20;
export const MOD_RALT   = 0x40;
export const MOD_RMETA  = 0x80;

export const CODE_TO_MOD = {
  ControlLeft:  MOD_LCTRL,
  ShiftLeft:    MOD_LSHIFT,
  AltLeft:      MOD_LALT,
  MetaLeft:     MOD_LMETA,
  ControlRight: MOD_RCTRL,
  ShiftRight:   MOD_RSHIFT,
  AltRight:     MOD_RALT,
  MetaRight:    MOD_RMETA,
};

export const CODE_TO_HID = {
  KeyA: 0x04, KeyB: 0x05, KeyC: 0x06, KeyD: 0x07, KeyE: 0x08, KeyF: 0x09,
  KeyG: 0x0A, KeyH: 0x0B, KeyI: 0x0C, KeyJ: 0x0D, KeyK: 0x0E, KeyL: 0x0F,
  KeyM: 0x10, KeyN: 0x11, KeyO: 0x12, KeyP: 0x13, KeyQ: 0x14, KeyR: 0x15,
  KeyS: 0x16, KeyT: 0x17, KeyU: 0x18, KeyV: 0x19, KeyW: 0x1A, KeyX: 0x1B,
  KeyY: 0x1C, KeyZ: 0x1D,

  Digit1: 0x1E, Digit2: 0x1F, Digit3: 0x20, Digit4: 0x21, Digit5: 0x22,
  Digit6: 0x23, Digit7: 0x24, Digit8: 0x25, Digit9: 0x26, Digit0: 0x27,

  Enter:        0x28,
  Escape:       0x29,
  Backspace:    0x2A,
  Tab:          0x2B,
  Space:        0x2C,
  Minus:        0x2D,
  Equal:        0x2E,
  BracketLeft:  0x2F,
  BracketRight: 0x30,
  Backslash:    0x31,
  Semicolon:    0x33,
  Quote:        0x34,
  Backquote:    0x35,
  Comma:        0x36,
  Period:       0x37,
  Slash:        0x38,
  CapsLock:     0x39,

  F1:  0x3A, F2:  0x3B, F3:  0x3C, F4:  0x3D, F5:  0x3E, F6:  0x3F,
  F7:  0x40, F8:  0x41, F9:  0x42, F10: 0x43, F11: 0x44, F12: 0x45,

  PrintScreen: 0x46,
  ScrollLock:  0x47,
  Pause:       0x48,
  Insert:      0x49,
  Home:        0x4A,
  PageUp:      0x4B,
  Delete:      0x4C,
  End:         0x4D,
  PageDown:    0x4E,
  ArrowRight:  0x4F,
  ArrowLeft:   0x50,
  ArrowDown:   0x51,
  ArrowUp:     0x52,

  NumpadEnter:    0x58,
  Numpad1:        0x59,
  Numpad2:        0x5A,
  Numpad3:        0x5B,
  Numpad4:        0x5C,
  Numpad5:        0x5D,
  Numpad6:        0x5E,
  Numpad7:        0x5F,
  Numpad8:        0x60,
  Numpad9:        0x61,
  Numpad0:        0x62,
  NumpadDecimal:  0x63,
  NumpadDivide:   0x54,
  NumpadMultiply: 0x55,
  NumpadSubtract: 0x56,
  NumpadAdd:      0x57,
};

const PLAIN_ASCII = {
  " ": 0x2C, "-": 0x2D, "=": 0x2E, "[": 0x2F, "]": 0x30, "\\": 0x31,
  ";": 0x33, "'": 0x34, "`": 0x35, ",": 0x36, ".": 0x37, "/": 0x38,
  "\n": 0x28, "\t": 0x2B,
};

const SHIFT_ASCII = {
  "!": 0x1E, "@": 0x1F, "#": 0x20, "$": 0x21, "%": 0x22, "^": 0x23,
  "&": 0x24, "*": 0x25, "(": 0x26, ")": 0x27, "_": 0x2D, "+": 0x2E,
  "{": 0x2F, "}": 0x30, "|": 0x31, ":": 0x33, "\"": 0x34, "~": 0x35,
  "<": 0x36, ">": 0x37, "?": 0x38,
};

export function asciiToHid(ch) {
  const code = ch.charCodeAt(0);
  if (code >= 97 && code <= 122) return { mod: 0, hid: 0x04 + (code - 97) };
  if (code >= 65 && code <= 90)  return { mod: MOD_LSHIFT, hid: 0x04 + (code - 65) };
  if (code >= 49 && code <= 57)  return { mod: 0, hid: 0x1E + (code - 49) };
  if (ch === "0")                return { mod: 0, hid: 0x27 };
  if (PLAIN_ASCII[ch])           return { mod: 0, hid: PLAIN_ASCII[ch] };
  if (SHIFT_ASCII[ch])           return { mod: MOD_LSHIFT, hid: SHIFT_ASCII[ch] };
  return null;
}

export function formatModBits(mod) {
  const parts = [];
  if (mod & (MOD_LCTRL | MOD_RCTRL))   parts.push("Ctrl");
  if (mod & (MOD_LSHIFT | MOD_RSHIFT)) parts.push("Shift");
  if (mod & (MOD_LALT | MOD_RALT))     parts.push("Alt");
  if (mod & (MOD_LMETA | MOD_RMETA))   parts.push("Cmd");
  return parts;
}
