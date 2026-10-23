#include "BleHidLink.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void clearEvent(BleHidEvent& event) {
  memset(&event, 0, sizeof(event));
}

static bool startsWith(const char* text, const char* prefix) {
  while (*prefix != '\0') {
    if (*text++ != *prefix++) {
      return false;
    }
  }
  return true;
}

static void copySpan(char* dest, size_t cap, const char* begin, const char* end) {
  size_t n = 0;
  if (begin == nullptr || end == nullptr || end < begin || cap == 0) {
    if (cap > 0) {
      dest[0] = '\0';
    }
    return;
  }
  while (begin < end && n + 1 < cap) {
    dest[n++] = *begin++;
  }
  while (n > 0 && dest[n - 1] == ' ') {
    n--;
  }
  dest[n] = '\0';
}

static void copyText(char* dest, size_t cap, const char* text) {
  if (text == nullptr) {
    text = "";
  }
  strncpy(dest, text, cap - 1);
  dest[cap - 1] = '\0';
}

static uint8_t modifierBit(const char* token) {
  if (strcmp(token, "LCTRL") == 0) return BleHidLink::MOD_LCTRL;
  if (strcmp(token, "LSHIFT") == 0) return BleHidLink::MOD_LSHIFT;
  if (strcmp(token, "LALT") == 0) return BleHidLink::MOD_LALT;
  if (strcmp(token, "LGUI") == 0) return BleHidLink::MOD_LGUI;
  if (strcmp(token, "RCTRL") == 0) return BleHidLink::MOD_RCTRL;
  if (strcmp(token, "RSHIFT") == 0) return BleHidLink::MOD_RSHIFT;
  if (strcmp(token, "RALT") == 0) return BleHidLink::MOD_RALT;
  if (strcmp(token, "RGUI") == 0) return BleHidLink::MOD_RGUI;
  return 0;
}

static uint8_t buttonBit(const char* token) {
  if (strcmp(token, "L") == 0) return BleHidLink::BTN_LEFT;
  if (strcmp(token, "R") == 0) return BleHidLink::BTN_RIGHT;
  if (strcmp(token, "M") == 0) return BleHidLink::BTN_MIDDLE;
  if (strcmp(token, "Back") == 0) return BleHidLink::BTN_BACK;
  if (strcmp(token, "Fwd") == 0) return BleHidLink::BTN_FORWARD;
  return 0;
}

static uint8_t bitsFromTokens(const char* text, uint8_t (*bitOf)(const char*)) {
  char buf[96];
  strncpy(buf, text, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  uint8_t bits = 0;
  char* token = strtok(buf, "+");
  while (token != nullptr) {
    bits |= bitOf(token);
    token = strtok(nullptr, "+");
  }
  return bits;
}

static bool labeledInt(const char* text, const char* key, long* out) {
  const char* found = strstr(text, key);
  if (found == nullptr) {
    return false;
  }
  found += strlen(key);
  char* end = nullptr;
  long value = strtol(found, &end, 10);
  if (end == found) {
    return false;
  }
  *out = value;
  return true;
}

BleHidLink::BleHidLink(Stream& port) : _port(&port), _len(0), _drop(false), _activity(false) {
  _line[0] = '\0';
}

void BleHidLink::onActivity() {
  _activity = true;
}

bool BleHidLink::takeActivity() {
  noInterrupts();
  bool pending = _activity;
  _activity = false;
  interrupts();
  return pending;
}

bool BleHidLink::poll(BleHidEvent& event) {
  while (_port->available() > 0) {
    char c = (char)_port->read();
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      _line[_len] = '\0';
      bool drop = _drop || _len == 0;
      _len = 0;
      _drop = false;
      if (drop) {
        continue;
      }
      return parseLine(event);
    }
    if (_len < sizeof(_line) - 1) {
      _line[_len++] = c;
    } else {
      _drop = true;
    }
  }
  return false;
}

void BleHidLink::send(const char* line) {
  if (line == nullptr) {
    return;
  }
  _port->print(line);
  _port->print('\n');
}

void BleHidLink::status() {
  send("status");
}

void BleHidLink::rescan() {
  send("rescan");
}

void BleHidLink::forget() {
  send("forget");
}

void BleHidLink::setPin(uint32_t pin) {
  char buf[24];
  snprintf(buf, sizeof(buf), "pin %lu", (unsigned long)pin);
  send(buf);
}

bool BleHidLink::parseLine(BleHidEvent& event) {
  clearEvent(event);

  const char* body = nullptr;
  if (startsWith(_line, "KBD")) {
    event.type = BLE_HID_KEYBOARD;
    body = _line + 3;
  } else if (startsWith(_line, "MOUSE")) {
    event.type = BLE_HID_MOUSE;
    body = _line + 5;
  } else if (startsWith(_line, "MEDIA")) {
    event.type = BLE_HID_MEDIA;
    body = _line + 5;
  } else if (startsWith(_line, "RAW")) {
    event.type = BLE_HID_RAW;
    body = _line + 3;
  } else {
    event.type = BLE_HID_INFO;
    copyText(event.text, sizeof(event.text), _line);
    return true;
  }

  while (*body == ' ') {
    body++;
  }

  const char* bracket = strstr(body, " [");
  const char* endBracket = bracket == nullptr ? nullptr : strchr(bracket + 2, ']');
  if (bracket == nullptr || endBracket == nullptr) {
    event.type = BLE_HID_INFO;
    copyText(event.text, sizeof(event.text), _line);
    return true;
  }

  copySpan(event.name, sizeof(event.name), body, bracket);
  copySpan(event.address, sizeof(event.address), bracket + 2, endBracket);

  const char* rest = endBracket + 1;
  if (*rest == ' ') {
    rest++;
  }
  copyText(event.text, sizeof(event.text), rest);
  event.released = strcmp(rest, "null") == 0;

  if (event.type == BLE_HID_KEYBOARD) {
    event.modifiers = bitsFromTokens(rest, modifierBit);
  } else if (event.type == BLE_HID_MOUSE) {
    const char* axes = strstr(rest, " x=");
    char buttons[24];
    if (axes != nullptr) {
      copySpan(buttons, sizeof(buttons), rest, axes);
    } else {
      copyText(buttons, sizeof(buttons), rest);
    }
    if (strcmp(buttons, "-") != 0) {
      event.buttons = bitsFromTokens(buttons, buttonBit);
    }
    long value = 0;
    if (labeledInt(rest, "x=", &value)) event.x = (int16_t)value;
    if (labeledInt(rest, "y=", &value)) event.y = (int16_t)value;
    if (labeledInt(rest, "wheel=", &value)) event.wheel = (int16_t)value;
    if (labeledInt(rest, "pan=", &value)) event.pan = (int16_t)value;
  }

  return true;
}
