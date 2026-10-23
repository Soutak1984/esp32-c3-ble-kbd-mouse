/*
  PrintEvents

  Reads the ESP32-C3 BLE HID host from a UART and prints each event
  on the USB serial port.

  Wiring, 3.3 V logic, common ground:
    ESP32-C3 GPIO4 TX -> this board RX
    ESP32-C3 GPIO5 RX <- this board TX
    ESP32-C3 GND      -> this board GND

  Baud must match kLinkBaud in esp32c3_ble_hid_host.ino.
  Hardware UART boards keep the default 115200.
  Uno and Nano use SoftwareSerial, which is not reliable at 115200.
  On those boards set kLinkBaud to 9600 and leave LINK_BAUD at 9600.

  Boards with Serial1 (Mega, Leonardo, Micro, ESP32, Pico, most ARM):
    the link uses Serial1.
  Uno and Nano:
    RX is pin 2, TX is pin 3.
*/

#include <BleHidLink.h>

#if defined(ARDUINO_ARCH_AVR) && !defined(HAVE_HWSERIAL1)
#include <SoftwareSerial.h>
SoftwareSerial linkPort(2, 3);  // RX, TX
static const unsigned long LINK_BAUD = 9600;
#else
// Serial1 on Mega, Leonardo, Micro, ESP32, Pico, and most ARM boards.
// ESP32 default pins differ by board. To choose them:
//   Serial1.begin(LINK_BAUD, SERIAL_8N1, RX_PIN, TX_PIN);
#define linkPort Serial1
static const unsigned long LINK_BAUD = 115200;
#endif

BleHidLink link(linkPort);

static const char* buttonName(uint8_t buttons) {
  if (buttons == 0) {
    return "-";
  }
  return nullptr;
}

void setup() {
  Serial.begin(115200);
  linkPort.begin(LINK_BAUD);
  Serial.println("BleHidLink example");
  Serial.println("Waiting for the ESP32-C3...");
}

void loop() {
  static bool asked = false;
  if (!asked && millis() > 2000) {
    asked = true;
    link.status();
  }

  BleHidEvent event;
  if (!link.poll(event)) {
    return;
  }

  switch (event.type) {
    case BLE_HID_KEYBOARD:
      Serial.print("KEY  ");
      Serial.print(event.name);
      Serial.print(" ");
      Serial.print(event.text);
      if (event.modifiers & BleHidLink::MOD_LCTRL) {
        Serial.print(" [ctrl]");
      }
      Serial.println();
      break;

    case BLE_HID_MOUSE:
      Serial.print("MOVE ");
      Serial.print(event.name);
      Serial.print(" buttons=");
      if (buttonName(event.buttons) != nullptr) {
        Serial.print("-");
      } else {
        if (event.buttons & BleHidLink::BTN_LEFT) Serial.print("L");
        if (event.buttons & BleHidLink::BTN_RIGHT) Serial.print("R");
        if (event.buttons & BleHidLink::BTN_MIDDLE) Serial.print("M");
        if (event.buttons & BleHidLink::BTN_BACK) Serial.print("Back");
        if (event.buttons & BleHidLink::BTN_FORWARD) Serial.print("Fwd");
      }
      Serial.print(" x=");
      Serial.print(event.x);
      Serial.print(" y=");
      Serial.print(event.y);
      Serial.print(" wheel=");
      Serial.print(event.wheel);
      Serial.print(" pan=");
      Serial.println(event.pan);
      break;

    case BLE_HID_MEDIA:
      Serial.print("MEDIA ");
      Serial.print(event.name);
      Serial.print(" ");
      Serial.println(event.text);
      break;

    case BLE_HID_RAW:
      Serial.print("RAW ");
      Serial.print(event.name);
      Serial.print(" ");
      Serial.println(event.text);
      break;

    default:
      Serial.println(event.text);
      break;
  }
}
