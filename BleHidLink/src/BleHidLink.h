#ifndef BLE_HID_LINK_H
#define BLE_HID_LINK_H

#include <Arduino.h>

// One parsed line from the ESP32-C3 BLE HID host.
enum BleHidType : uint8_t {
  BLE_HID_INFO = 0,
  BLE_HID_KEYBOARD,
  BLE_HID_MOUSE,
  BLE_HID_MEDIA,
  BLE_HID_RAW
};

struct BleHidEvent {
  BleHidType type;
  char name[32];
  char address[18];
  char text[96];
  uint8_t modifiers;
  uint8_t buttons;
  int16_t x;
  int16_t y;
  int16_t wheel;
  int16_t pan;
  bool released;
};

// Reads the link UART and turns each line into a BleHidEvent.
// The Stream is the serial port wired to the ESP32-C3.
class BleHidLink {
 public:
  static const uint8_t BTN_LEFT = 0x01;
  static const uint8_t BTN_RIGHT = 0x02;
  static const uint8_t BTN_MIDDLE = 0x04;
  static const uint8_t BTN_BACK = 0x08;
  static const uint8_t BTN_FORWARD = 0x10;

  static const uint8_t MOD_LCTRL = 0x01;
  static const uint8_t MOD_LSHIFT = 0x02;
  static const uint8_t MOD_LALT = 0x04;
  static const uint8_t MOD_LGUI = 0x08;
  static const uint8_t MOD_RCTRL = 0x10;
  static const uint8_t MOD_RSHIFT = 0x20;
  static const uint8_t MOD_RALT = 0x40;
  static const uint8_t MOD_RGUI = 0x80;

  explicit BleHidLink(Stream& port);

  // Read whatever has arrived. Returns true when event is a complete line.
  bool poll(BleHidEvent& event);

  void status();
  void rescan();
  void forget();
  void setPin(uint32_t pin);
  void send(const char* line);

  // The ESP32-C3 activity pin pulses high on a key press or a mouse move.
  // Call this from the interrupt on the GPIO wired to that pin.
  void onActivity();
  // True once after onActivity, then the flag is cleared.
  bool takeActivity();

 private:
  Stream* _port;
  char _line[160];
  uint8_t _len;
  bool _drop;
  volatile bool _activity;

  bool parseLine(BleHidEvent& event);
};

#endif
