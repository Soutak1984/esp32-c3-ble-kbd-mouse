# ESP32-C3 BLE keyboard and mouse host

The ESP32-C3 connects to one Bluetooth LE keyboard and one Bluetooth LE mouse, one after the other, then prints every key, mouse move, click, and media key on serial.

The same lines go out on the USB serial port and on a UART you can wire to another microcontroller.

The ESP32-C3 radio is Bluetooth LE only. A classic Bluetooth (BR/EDR) keyboard or mouse will not connect. The device must advertise as BLE.

## What you need

- An ESP32-C3 board
- Arduino IDE
- Board package: **esp32** by Espressif Systems, 3.x
- Library: **NimBLE-Arduino** by h2zero, version 2.x
- A BLE keyboard and a BLE mouse

Install the library from **Tools > Manage Libraries**. Search for `NimBLE-Arduino`.

## Arduino IDE setup

1. Open the folder `esp32c3_ble_hid_host`. The sketch file is `esp32c3_ble_hid_host.ino`.
2. **Tools > Board > ESP32C3 Dev Module**
3. **Tools > USB CDC On Boot > Enabled** on native-USB boards such as the ESP32-C3 Super Mini and XIAO ESP32-C3. A devkit with a USB-UART chip can leave CDC disabled and use that port.
4. Select the port that appears when the board is plugged in.
5. Upload.
6. Open the Serial Monitor at **115200**.

## Pairing

Unpair the keyboard and mouse from the computer before you use them here. A device that is already linked to a PC stops advertising, so the ESP32-C3 cannot see it.

The board connects them one by one. Either device can be first.

1. Put the first device in pairing mode and reset the ESP32-C3.
2. Wait for `Keyboard ready` or `Mouse ready`. The log then names the device that is still missing and keeps scanning.
3. Put the second device in pairing mode. You do not press BOOT between these two. The startup scan is still running.
4. When the second device is ready, the log prints `Receiving...` and scanning stops.

The host pairs with Just Works (no PIN) and stores the bond. After a reset it looks for that same keyboard and that same mouse again, still one at a time.

It keeps one keyboard and one mouse. It does not connect a second keyboard or a second mouse. A keyboard with a touchpad can fill both roles by itself. In that case the log says the device is both, and scanning stops.

If a device disconnects after both were connected, scanning does not start by itself. Press the BOOT button (GPIO9, active low) or type `rescan`. That looks only for the keyboard or mouse that is missing. Turn the old device off first if you want a different one in its place. While both are already connected, BOOT and `rescan` do not add another device.

A startup log looks like this when the keyboard is found first:

```text
ESP32-C3 BLE HID host
This board connects TO one BLE keyboard and one BLE mouse.
Scanning for a BLE keyboard and mouse...
Found Logitech K380 [aa:bb:cc:dd:ee:ff] appearance=0x03C1
Keyboard ready: Logitech K380
Mouse is still missing.
Scanning for the mouse...
Found MX Anywhere [11:22:33:44:55:66] appearance=0x03C2
Mouse ready: MX Anywhere
Receiving. Type on the keyboard or move the mouse.
```

After a disconnect, BOOT prints:

```text
BOOT pressed.
Looking for a mouse.
Scanning for the mouse...
```

## Output lines

USB serial and the link UART print the same lines. Each line ends with a newline.

```text
KBD   Logitech K380 [aa:bb:cc:dd:ee:ff] LSHIFT+a
KBD   Logitech K380 [aa:bb:cc:dd:ee:ff] (up)
MOUSE MX Anywhere [11:22:33:44:55:66] L x=3 y=-1 wheel=0 pan=0
MEDIA Logitech K380 [aa:bb:cc:dd:ee:ff] VolumeUp
RAW   Name [addr] 01 02 00
```

| Prefix | Meaning |
| --- | --- |
| `KBD` | Keyboard. Modifiers and keys are joined with `+`. `(up)` means every key was released. |
| `MOUSE` | Buttons (`L`, `R`, `M`, `Back`, `Fwd`, or `-`), then `x`, `y`, `wheel`, and `pan`. Movement is grouped about every 40 ms. Clicks and the wheel print immediately. |
| `MEDIA` | Volume, play, and other consumer keys. `(up)` is the release. |
| `RAW` | A HID report the sketch could not decode. The bytes are hex. |

`NKRO` after `KBD` means the keyboard sent a bitfield report longer than the normal 6-key report. Those bytes are printed in hex.

## Commands

Type these in the Serial Monitor, or send them on the link UART. End each command with a newline.

| Command | Action |
| --- | --- |
| `status` | Show which devices are connected, which one is still missing, and how many bonds are stored. |
| `rescan` | Scan for a missing keyboard or mouse, including devices that were skipped. Does nothing while both are already connected. |
| `forget` | Erase stored BLE bonds and restart. |
| `pin 123456` | Passkey to send the next time a device asks for one. |
| `help` | Print the command list. |

## Link UART

A second serial port repeats every line so another microcontroller can read them.

Default wiring:

| ESP32-C3 | Other MCU |
| --- | --- |
| GPIO4 (TX) | RX |
| GPIO5 (RX) | TX |
| GND | GND |

Settings are **115200 baud, 8 data bits, no parity, 1 stop bit**.

Both chips use 3.3 V logic. A 5 V microcontroller needs a level shifter on the wire that goes into the ESP32 RX pin.

Change the pins and baud near the top of the sketch:

```cpp
static const bool kLinkUart = true;
static const int kLinkTxPin = 4;
static const int kLinkRxPin = 5;
static const uint32_t kLinkBaud = 115200;
```

Any free GPIO can be TX or RX. Leave GPIO18 and GPIO19 free while USB is in use. Set `kLinkRxPin` to `-1` if the other microcontroller only listens.

After boot, the log prints the pins it is using:

```text
Link UART: TX GPIO4, RX GPIO5, 115200 baud
```

## Settings

These are at the top of `esp32c3_ble_hid_host.ino`.

| Setting | Purpose |
| --- | --- |
| `kKeyboardName` | Part of the keyboard's BLE name. Empty accepts the first keyboard. |
| `kMouseName` | Part of the mouse's BLE name. Empty accepts the first mouse. |
| `kUsePasskey` | Set to `true` when a device rejects Just Works pairing. |
| `kPasskey` | Six-digit code sent when `kUsePasskey` is true. Default is `123456`. |
| `kLinkUart` | `true` enables the GPIO UART. |
| `kLinkTxPin`, `kLinkRxPin`, `kLinkBaud` | UART pins and speed. |
| `kBootPin` | BOOT button pin. Default is GPIO9, active low. Set to `-1` to disable it. |

A keyboard that also has a touchpad can fill both roles on one connection. Media keys on a keyboard are printed as `MEDIA` lines.

## Other microcontroller

The folder `BleHidLink` is an Arduino library for the board on the other side of the link UART. It reads the text lines and fills a `BleHidEvent` with the key chord, mouse axes, buttons, and media key.

Install it by copying `BleHidLink` into the Arduino `libraries` folder, then restart the IDE. The example is **File > Examples > BleHidLink > PrintEvents**.

```cpp
#include <BleHidLink.h>

BleHidLink link(Serial1);

void setup() {
  Serial1.begin(115200);
}

void loop() {
  BleHidEvent event;
  if (!link.poll(event)) {
    return;
  }
  if (event.type == BLE_HID_KEYBOARD && !event.released) {
    Serial.println(event.text);
  }
}
```

`link.status()`, `link.rescan()`, `link.forget()`, and `link.setPin(123456)` send the matching command back to the ESP32-C3.

On an Uno or Nano the example uses SoftwareSerial on pins 2 and 3 at 9600 baud. Set `kLinkBaud` in the ESP32 sketch to `9600` as well. Mega, Leonardo, ESP32, Pico, and most ARM boards use `Serial1` at 115200.

## If it does not connect

On startup the board connects one device, then scans for the other. `Receiving...` is printed only when both are up. A later search starts only when you press BOOT or type `rescan`, and only for the role that is missing.

- Confirm both devices are BLE. Classic Bluetooth hardware will never show up.
- Remove both devices from the computer's Bluetooth list. A device that stays connected to a PC stops advertising.
- Pair them one by one. Reset the board, wait until the first `ready` line, then put the other device in pairing mode.
- Watch for a `Found ...` line for each device. `Mouse is still missing.` or `Keyboard is still missing.` means the first one is connected and the scan for the other is still running.
- If pairing fails, type `forget`, pair again, or set `kUsePasskey` to `true` and upload.
- If the wrong nearby device is taken, set `kKeyboardName` or `kMouseName` to part of the name printed in the log.
- `status` shows whether the keyboard, the mouse, or both are still missing.
- If the boot log says `BLE connections available: 1`, this NimBLE build can hold only one link. Install NimBLE-Arduino 2.x with the ESP32 3.x board package, then upload again.
- If one device is reported as both a keyboard and a mouse, scanning stops on purpose. That is a combo device (keyboard with a touchpad). A separate mouse is not required in that case.
