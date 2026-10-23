/*
 * ESP32-C3 BLE HID host
 * Connects to one Bluetooth LE keyboard and one Bluetooth LE mouse at the
 * same time and prints what they send on Serial at 115200. A second
 * keyboard or a second mouse is not connected. Press the BOOT button to
 * scan for a replacement.
 *
 * The ESP32-C3 radio is BLE only. A classic Bluetooth (BR/EDR) keyboard or
 * mouse will not connect. The device must be in BLE / "Bluetooth LE" mode,
 * and it must be advertising. Unpair it from the computer first, then put
 * it in pairing mode. A device that is already linked to a PC usually
 * stops advertising, so this board cannot see it.
 *
 * Board package: esp32 by Espressif Systems, 3.x
 *   Tools > Board > ESP32C3 Dev Module
 *   Tools > USB CDC On Boot > Enabled
 *     (native-USB boards such as ESP32-C3 Super Mini and XIAO ESP32-C3)
 *     Devkits with a USB-UART chip can leave CDC disabled and use that port.
 *   Tools > Port > the port that appears when the board is plugged in
 *
 * Library (required): NimBLE-Arduino by h2zero, version 2.x
 *   Tools > Manage Libraries > search "NimBLE-Arduino" > Install
 *
 * Open the Serial Monitor at 115200. Commands:
 *   status          print connection state
 *   rescan          look again, including devices that were skipped
 *   forget          erase stored BLE bonds and reboot
 *   pin 123456      passkey to send the next time a device asks
 *
 * Optional filters. Leave a string empty to take the first match.
 * Set kUsePasskey if the keyboard will not pair with "Just Works".
 *
 * Pins (change the matching setting below, or set it to -1 to disable):
 *   GPIO4   kLinkTxPin       UART TX to the other MCU RX. 115200 8N1.
 *   GPIO5   kLinkRxPin       UART RX from the other MCU TX. Share ground.
 *   GPIO6   kActivityPin     Output. High for 20 ms on a key press or mouse move.
 *   GPIO7   kHexPin          Input, pull-up on. Open: typed characters.
 *                            Tied to ground: print the pressed key as hex.
 *   GPIO9   kBootPin         BOOT button, active low.
 *                            Short press: pair a new keyboard or mouse.
 *                            Hold 5 seconds: forget every paired device.
 *   GPIO10  kKeyboardLedPin  Output. High while a keyboard is connected.
 *   GPIO3   kMouseLedPin     Output. High while a mouse is connected.
 *   GPIO18  USB D-           Leave free while USB is in use.
 *   GPIO19  USB D+           Leave free while USB is in use.
 *
 * Each LED goes from its pin to ground through a resistor (about 330 ohm
 * to 1 kohm). A keyboard that is also a mouse lights both LEDs.
 * Both UART sides are 3.3 V logic.
 */

#include <Arduino.h>
#include <NimBLEDevice.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

// ---- user settings ----------------------------------------------------------

static const char kKeyboardName[] = "";  // substring of the keyboard name, or ""
static const char kMouseName[] = "";     // substring of the mouse name, or ""
static const bool kUsePasskey = false;   // true: send kPasskey when asked
static const uint32_t kPasskey = 123456;

// Second serial port for another microcontroller.
// ESP32 TX goes to the other MCU RX. ESP32 RX comes from the other MCU TX.
// Share ground. Both sides are 3.3 V logic. Change the pins to any free GPIO.
// Set kLinkRxPin to -1 if the other MCU only listens.
// Leave GPIO18 and GPIO19 alone while USB is in use.
static const bool kLinkUart = true;
static const int kLinkTxPin = 4;
static const int kLinkRxPin = 5;
static const uint32_t kLinkBaud = 115200;

// ESP32-C3 BOOT button. It is GPIO9, active low, on DevKit and Super Mini
// boards. Press it to scan for a keyboard or mouse that is not connected.
// Set this to -1 if the board has no BOOT button on a GPIO.
// A short press pairs a new device. Hold for kBootForgetMs to erase bonds.
static const int kBootPin = 9;
static const uint32_t kBootForgetMs = 5000;

// Activity output for the other microcontroller. Idle is low.
// It goes high for kActivityPulseMs on a key press or a mouse move.
// Set kActivityPin to -1 to disable it. Leave GPIO18 and GPIO19 free for USB.
static const int kActivityPin = 6;
static const uint32_t kActivityPulseMs = 20;

// Hex mode input. Internal pull-up is on, so leave the pin open for normal
// characters. Tie it to ground to print each pressed key as a hex usage
// instead, such as 1D for z and E0 for left Control. Set to -1 to disable.
static const int kHexPin = 7;

// Status LEDs. Each pin goes high while that device is connected, and low
// when it is not. Wire the LED from the pin to ground through a resistor.
// A keyboard that is also a mouse lights both. Set either pin to -1 to
// disable it. Leave GPIO18 and GPIO19 free for USB.
static const int kKeyboardLedPin = 10;
static const int kMouseLedPin = 3;

// ---- HID identifiers --------------------------------------------------------

static const uint16_t kHidService = 0x1812;
static const uint16_t kReportMap = 0x2A4B;
static const uint16_t kHidReport = 0x2A4D;
static const uint16_t kProtocolMode = 0x2A4E;
static const uint16_t kHidControlPoint = 0x2A4C;
static const uint16_t kBootKeyboard = 0x2A22;
static const uint16_t kBootKeyboardOutput = 0x2A32;
static const uint16_t kBootMouse = 0x2A33;
static const uint16_t kReportRef = 0x2908;

static const uint16_t kAppearanceKeyboard = 0x03C1;
static const uint16_t kAppearanceMouse = 0x03C2;

static const uint8_t kKindUnknown = 0;
static const uint8_t kKindKeyboard = 1;
static const uint8_t kKindMouse = 2;
static const uint8_t kKindConsumer = 3;

static const int kSlotCount = 2;
static const int kMaxReports = 8;
static const int kMaxIds = 6;
static const int kMaxMemory = 16;

// ---- state ------------------------------------------------------------------

struct ReportMeta {
  uint16_t handle;
  uint8_t reportId;
  uint8_t kind;
  bool idInPayload;
};

struct IdKind {
  uint8_t id;
  uint8_t kind;
};

struct Slot {
  bool used;
  bool hasKeyboard;
  bool hasMouse;
  NimBLEClient* client;
  char name[32];
  char addr[18];
  char advert[18];
  uint16_t appearance;
  bool mapUsesReportIds;
  IdKind ids[kMaxIds];
  uint8_t idCount;
  ReportMeta reports[kMaxReports];
  uint8_t reportCount;
  bool haveLastKey;
  bool capsLock;
  bool numLock;
  bool scrollLock;
  bool haveLedId;
  uint8_t ledMapId;
  bool ledDirty;
  bool ledFailNoted;
  uint32_t ledNextMs;
  NimBLERemoteCharacteristic* ledOutput;
  NimBLERemoteCharacteristic* bootLed;
  uint8_t lastMod;
  uint8_t lastKeys[6];
  bool haveLastMouse;
  uint8_t lastButtons;
  int32_t accX;
  int32_t accY;
  int32_t accWheel;
  int32_t accPan;
  uint32_t lastMoveMs;
  bool haveLastMedia;
  uint16_t lastMedia;
};

// role: 0 unused, 1 keyboard, 2 mouse, 3 both, 4 not a keyboard or mouse
struct Remembered {
  bool used;
  char addr[18];
  uint8_t role;
};

struct LineEvent {
  char text[160];
};

static Slot gSlots[kSlotCount];
static Remembered gMemory[kMaxMemory];
static QueueHandle_t gLines = nullptr;
static const uint8_t kScanIdle = 0;
static const uint8_t kScanRunning = 1;
static const uint8_t kScanStopping = 2;

static const NimBLEAdvertisedDevice* gPending = nullptr;
static volatile uint32_t gPendingMs = 0;
static volatile bool gConnecting = false;
static volatile uint8_t gScanState = kScanIdle;
static volatile uint32_t gScanStateMs = 0;
static volatile bool gWantScan = false;
static uint32_t gNextScanMs = 0;
static int gScanNote = -1;
static bool gLinksRelaxed = false;
static bool gAllowScan = false;
static bool gPairNew = false;
static char gUsbCmd[48];
static uint8_t gUsbCmdLen = 0;
static char gLinkCmd[48];
static uint8_t gLinkCmdLen = 0;
static volatile uint32_t gActivityUntil = 0;
static volatile uint32_t gRuntimePasskey = 0;
bool gReadyNoted = false;
bool gBleReady = false;

class ClientCallbacks : public NimBLEClientCallbacks {
 public:
  void onConnect(NimBLEClient* pClient) override;
  void onConnectFail(NimBLEClient* pClient, int reason) override;
  void onDisconnect(NimBLEClient* pClient, int reason) override;
  void onPassKeyEntry(NimBLEConnInfo& connInfo) override;
  void onConfirmPasskey(NimBLEConnInfo& connInfo, uint32_t pin) override;
  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override;
};

class ScanCallbacks : public NimBLEScanCallbacks {
 public:
  void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override;
  void onScanEnd(const NimBLEScanResults& results, int reason) override;
};

static ClientCallbacks gClientCallbacks;
static ScanCallbacks gScanCallbacks;

// ---- small helpers ----------------------------------------------------------

void emitLine(const char* text) {
  Serial.println(text);
  if (kLinkUart) {
    Serial1.println(text);
  }
}

void emitf(const char* fmt, ...) {
  char buf[180];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  emitLine(buf);
}

void enqueueLine(const char* text) {
  if (gLines == nullptr) {
    emitLine(text);
    return;
  }
  LineEvent event;
  strncpy(event.text, text, sizeof(event.text) - 1);
  event.text[sizeof(event.text) - 1] = '\0';
  xQueueSend(gLines, &event, 0);
}

void enqueuef(const char* fmt, ...) {
  char buf[160];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  enqueueLine(buf);
}

void drainLines() {
  if (gLines == nullptr) {
    return;
  }
  LineEvent event;
  while (xQueueReceive(gLines, &event, 0) == pdTRUE) {
    emitLine(event.text);
  }
}

void logf(const char* fmt, ...) {
  char buf[180];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  emitLine(buf);
}

void copyText(char* dest, size_t cap, const std::string& src, const char* fallback) {
  const char* in = src.empty() ? fallback : src.c_str();
  size_t j = 0;
  if (in == nullptr) {
    in = "HID";
  }
  for (size_t i = 0; in[i] != '\0' && j + 1 < cap; i++) {
    unsigned char c = (unsigned char)in[i];
    dest[j++] = (c < 32 || c == 127) ? ' ' : (char)c;
  }
  if (j == 0) {
    strncpy(dest, "HID", cap - 1);
    dest[cap - 1] = '\0';
    return;
  }
  dest[j] = '\0';
}

bool containsText(const std::string& hay, const char* needle) {
  if (needle == nullptr || needle[0] == '\0' || hay.empty()) {
    return false;
  }
  size_t needleLen = strlen(needle);
  if (needleLen > hay.size()) {
    return false;
  }
  for (size_t i = 0; i + needleLen <= hay.size(); i++) {
    bool match = true;
    for (size_t j = 0; j < needleLen; j++) {
      char a = hay[i + j];
      char b = needle[j];
      if (a >= 'A' && a <= 'Z') {
        a = (char)(a - 'A' + 'a');
      }
      if (b >= 'A' && b <= 'Z') {
        b = (char)(b - 'A' + 'a');
      }
      if (a != b) {
        match = false;
        break;
      }
    }
    if (match) {
      return true;
    }
  }
  return false;
}

bool nameAllowed(const std::string& name, const char* filter) {
  if (filter == nullptr || filter[0] == '\0') {
    return true;
  }
  return containsText(name, filter);
}

void clearSlot(int index) {
  Slot& slot = gSlots[index];
  slot.used = false;
  slot.hasKeyboard = false;
  slot.hasMouse = false;
  slot.client = nullptr;
  slot.name[0] = '\0';
  slot.addr[0] = '\0';
  slot.advert[0] = '\0';
  slot.appearance = 0;
  slot.mapUsesReportIds = false;
  slot.idCount = 0;
  slot.reportCount = 0;
  slot.haveLastKey = false;
  slot.capsLock = false;
  slot.numLock = false;
  slot.scrollLock = false;
  slot.haveLedId = false;
  slot.ledMapId = 0;
  slot.ledDirty = false;
  slot.ledFailNoted = false;
  slot.ledNextMs = 0;
  slot.ledOutput = nullptr;
  slot.bootLed = nullptr;
  slot.lastMod = 0;
  memset(slot.lastKeys, 0, sizeof(slot.lastKeys));
  slot.haveLastMouse = false;
  slot.lastButtons = 0;
  slot.accX = 0;
  slot.accY = 0;
  slot.accWheel = 0;
  slot.accPan = 0;
  slot.lastMoveMs = 0;
  slot.haveLastMedia = false;
  slot.lastMedia = 0;
}

int findSlotByClient(NimBLEClient* client) {
  if (client == nullptr) {
    return -1;
  }
  for (int i = 0; i < kSlotCount; i++) {
    if (gSlots[i].used && gSlots[i].client == client) {
      return i;
    }
  }
  return -1;
}

int findFreeSlot() {
  for (int i = 0; i < kSlotCount; i++) {
    if (!gSlots[i].used) {
      return i;
    }
  }
  return -1;
}

bool haveKeyboard() {
  for (int i = 0; i < kSlotCount; i++) {
    if (gSlots[i].used && gSlots[i].hasKeyboard) {
      return true;
    }
  }
  return false;
}

bool haveMouse() {
  for (int i = 0; i < kSlotCount; i++) {
    if (gSlots[i].used && gSlots[i].hasMouse) {
      return true;
    }
  }
  return false;
}

bool stillNeed() {
  return !haveKeyboard() || !haveMouse();
}

int connectedCount() {
  int count = 0;
  for (int i = 0; i < kSlotCount; i++) {
    if (gSlots[i].used && gSlots[i].client != nullptr && gSlots[i].client->isConnected()) {
      count++;
    }
  }
  return count;
}

bool textEq(const char* a, const char* b) {
  return a != nullptr && b != nullptr && a[0] != '\0' && strcmp(a, b) == 0;
}

int findMemory(const char* addr) {
  for (int i = 0; i < kMaxMemory; i++) {
    if (gMemory[i].used && strcmp(gMemory[i].addr, addr) == 0) {
      return i;
    }
  }
  return -1;
}

void rememberDevice(const char* addr, uint8_t role) {
  if (addr == nullptr || addr[0] == '\0') {
    return;
  }
  int existing = findMemory(addr);
  if (existing >= 0) {
    gMemory[existing].role = role;
    return;
  }
  for (int i = 0; i < kMaxMemory; i++) {
    if (!gMemory[i].used) {
      strncpy(gMemory[i].addr, addr, sizeof(gMemory[i].addr) - 1);
      gMemory[i].addr[sizeof(gMemory[i].addr) - 1] = '\0';
      gMemory[i].role = role;
      gMemory[i].used = true;
      return;
    }
  }
}

void clearMemory() {
  for (int i = 0; i < kMaxMemory; i++) {
    gMemory[i].used = false;
    gMemory[i].role = 0;
    gMemory[i].addr[0] = '\0';
  }
}

bool sameAddrText(const char* addr) {
  for (int i = 0; i < kSlotCount; i++) {
    if (!gSlots[i].used) {
      continue;
    }
    if (textEq(gSlots[i].addr, addr) || textEq(gSlots[i].advert, addr)) {
      return true;
    }
  }
  return false;
}

void rememberKind(int slotIndex, uint8_t id, uint8_t kind) {
  if (kind == kKindUnknown) {
    return;
  }
  Slot& slot = gSlots[slotIndex];
  for (uint8_t i = 0; i < slot.idCount; i++) {
    if (slot.ids[i].id == id) {
      if (slot.ids[i].kind == kKindUnknown) {
        slot.ids[i].kind = kind;
      }
      return;
    }
  }
  if (slot.idCount >= kMaxIds) {
    return;
  }
  slot.ids[slot.idCount].id = id;
  slot.ids[slot.idCount].kind = kind;
  slot.idCount++;
}

uint8_t kindForId(int slotIndex, uint8_t id) {
  const Slot& slot = gSlots[slotIndex];
  for (uint8_t i = 0; i < slot.idCount; i++) {
    if (slot.ids[i].id == id) {
      return slot.ids[i].kind;
    }
  }
  if (!slot.mapUsesReportIds && slot.idCount == 1) {
    return slot.ids[0].kind;
  }
  return kKindUnknown;
}

void addReport(int slotIndex, uint16_t handle, uint8_t reportId, uint8_t kind, bool idInPayload) {
  Slot& slot = gSlots[slotIndex];
  if (slot.reportCount >= kMaxReports) {
    return;
  }
  ReportMeta& report = slot.reports[slot.reportCount++];
  report.handle = handle;
  report.reportId = reportId;
  report.kind = kind;
  report.idInPayload = idInPayload;
  if (kind == kKindKeyboard) {
    slot.hasKeyboard = true;
  } else if (kind == kKindMouse) {
    slot.hasMouse = true;
  }
}

int findReport(int slotIndex, uint16_t handle) {
  const Slot& slot = gSlots[slotIndex];
  for (uint8_t i = 0; i < slot.reportCount; i++) {
    if (slot.reports[i].handle == handle) {
      return i;
    }
  }
  return -1;
}

// Walk a HID report descriptor far enough to tell keyboard, mouse, and
// consumer-control collections apart, and which report ID each one uses.
void parseReportMap(int slotIndex, const uint8_t* data, size_t length) {
  uint16_t usagePage = 0;
  uint8_t reportId = 0;
  uint16_t pendingUsage = 0;
  uint16_t pendingPage = 0;
  uint16_t appPage = 0;
  uint16_t appUsage = 0;
  int depth = 0;
  struct Level {
    uint16_t page;
    uint16_t usage;
    bool isApp;
  } stack[8];

  size_t i = 0;
  while (i < length) {
    uint8_t prefix = data[i++];
    if (prefix == 0xFE) {
      if (i + 2 > length) {
        break;
      }
      uint8_t dataSize = data[i++];
      i++;
      if (i + dataSize > length) {
        break;
      }
      i += dataSize;
      continue;
    }

    uint8_t sizeCode = prefix & 0x03;
    uint8_t size = sizeCode == 3 ? 4 : sizeCode;
    uint8_t type = (prefix >> 2) & 0x03;
    uint8_t tag = (prefix >> 4) & 0x0F;
    uint32_t value = 0;
    if (i + size > length) {
      break;
    }
    for (uint8_t b = 0; b < size; b++) {
      value |= (uint32_t)data[i++] << (8 * b);
    }

    if (type == 1) {
      if (tag == 0) {
        usagePage = (uint16_t)value;
      } else if (tag == 8) {
        reportId = (uint8_t)value;
        gSlots[slotIndex].mapUsesReportIds = true;
      }
    } else if (type == 2 && tag == 0) {
      if (size == 4) {
        pendingPage = (uint16_t)((value >> 16) & 0xFFFF);
        pendingUsage = (uint16_t)(value & 0xFFFF);
      } else {
        pendingPage = usagePage;
        pendingUsage = (uint16_t)value;
      }
    } else if (type == 0 && tag == 0xA) {
      uint16_t page = pendingPage ? pendingPage : usagePage;
      if (depth < 8) {
        stack[depth].page = page;
        stack[depth].usage = pendingUsage;
        stack[depth].isApp = (value == 1);
      }
      if (value == 1) {
        appPage = page;
        appUsage = pendingUsage;
      }
      if (depth < 8) {
        depth++;
      }
      pendingUsage = 0;
      pendingPage = 0;
    } else if (type == 0 && tag == 0xC) {
      if (depth > 0) {
        depth--;
      }
      appPage = 0;
      appUsage = 0;
      for (int level = depth - 1; level >= 0; level--) {
        if (stack[level].isApp) {
          appPage = stack[level].page;
          appUsage = stack[level].usage;
          break;
        }
      }
    } else if (type == 0 && tag == 0x8) {
      bool constant = (value & 0x01) != 0;
      if (!constant) {
        uint8_t kind = kKindUnknown;
        if ((appPage == 0x01 && appUsage == 0x06) || appPage == 0x07) {
          kind = kKindKeyboard;
        } else if (appPage == 0x01 && appUsage == 0x02) {
          kind = kKindMouse;
        } else if (appPage == 0x0C) {
          kind = kKindConsumer;
        }
        rememberKind(slotIndex, reportId, kind);
      }
      pendingUsage = 0;
      pendingPage = 0;
    } else if (type == 0 && tag == 0x9) {
      // Output report. Keyboard lock LEDs live on the LED usage page.
      bool constant = (value & 0x01) != 0;
      if (!constant) {
        uint16_t page = pendingPage ? pendingPage : usagePage;
        bool leds = page == 0x08;
        bool keyboard = (appPage == 0x01 && appUsage == 0x06) || appPage == 0x07;
        if (leds || (keyboard && !gSlots[slotIndex].haveLedId)) {
          gSlots[slotIndex].haveLedId = true;
          gSlots[slotIndex].ledMapId = reportId;
        }
      }
      pendingUsage = 0;
      pendingPage = 0;
    }
  }
}

void notifyCB(NimBLERemoteCharacteristic* chr, uint8_t* data, size_t length, bool isNotify);

bool writeByte(NimBLERemoteCharacteristic* chr, uint8_t value) {
  if (chr == nullptr) {
    return false;
  }
  if (chr->writeValue(&value, 1, false)) {
    return true;
  }
  return chr->writeValue(&value, 1, true);
}

bool canWriteReport(NimBLERemoteCharacteristic* chr) {
  return chr != nullptr && (chr->canWrite() || chr->canWriteNoResponse());
}

bool writeBytes(NimBLERemoteCharacteristic* chr, const uint8_t* data, size_t length) {
  if (!canWriteReport(chr) || data == nullptr || length == 0) {
    return false;
  }
  if (chr->writeValue(data, length, false)) {
    return true;
  }
  return chr->writeValue(data, length, true);
}

uint8_t keyboardLedBits(int slotIndex) {
  const Slot& slot = gSlots[slotIndex];
  uint8_t bits = 0;
  if (slot.numLock) {
    bits |= 0x01;
  }
  if (slot.capsLock) {
    bits |= 0x02;
  }
  if (slot.scrollLock) {
    bits |= 0x04;
  }
  return bits;
}

bool writeKeyboardLeds(int slotIndex) {
  Slot& slot = gSlots[slotIndex];
  if (slot.client == nullptr || !slot.client->isConnected()) {
    return false;
  }
  uint8_t bits = keyboardLedBits(slotIndex);
  // One byte only. Bit 0 is Num Lock, bit 1 is Caps Lock, bit 2 is Scroll Lock.
  // The report reference already names the report. Putting the report ID in
  // front makes this keyboard use that ID as the LED byte, so ID 1 latches
  // Num Lock on and later writes cannot turn it off.
  bool wrote = false;
  if (slot.ledOutput != nullptr) {
    wrote = writeBytes(slot.ledOutput, &bits, 1);
  }
  if (!wrote && slot.bootLed != nullptr) {
    wrote = writeBytes(slot.bootLed, &bits, 1);
  }
  return wrote;
}

void serviceKeyboardLeds() {
  uint32_t now = millis();
  for (int i = 0; i < kSlotCount; i++) {
    Slot& slot = gSlots[i];
    if (!slot.used || !slot.ledDirty) {
      continue;
    }
    if ((int32_t)(now - slot.ledNextMs) < 0) {
      continue;
    }
    if (slot.ledOutput == nullptr && slot.bootLed == nullptr) {
      slot.ledDirty = false;
      continue;
    }
    uint8_t bits = keyboardLedBits(i);
    if (!writeKeyboardLeds(i)) {
      slot.ledNextMs = now + 300;
      if (!slot.ledFailNoted) {
        slot.ledFailNoted = true;
        logf("Keyboard lock LEDs did not accept the update.");
      }
      continue;
    }
    if (keyboardLedBits(i) == bits) {
      slot.ledDirty = false;
      logf("Keyboard LEDs %02X.", bits);
    }
  }
}

bool subscribeInput(NimBLERemoteCharacteristic* chr) {
  if (chr == nullptr) {
    return false;
  }
  if (chr->canNotify()) {
    return chr->subscribe(true, notifyCB);
  }
  if (chr->canIndicate()) {
    return chr->subscribe(false, notifyCB);
  }
  return false;
}

// ---- report formatting ------------------------------------------------------

void appendPiece(char* buf, size_t cap, size_t& used, const char* piece, bool withPlus) {
  if (used + 1 >= cap) {
    return;
  }
  if (withPlus && used > 0) {
    buf[used++] = '+';
    buf[used] = '\0';
  }
  int wrote = snprintf(buf + used, cap - used, "%s", piece);
  if (wrote < 0) {
    return;
  }
  if ((size_t)wrote >= cap - used) {
    used = cap - 1;
  } else {
    used += (size_t)wrote;
  }
}

bool codeInKeys(const uint8_t* codes, uint8_t code) {
  for (int i = 0; i < 6; i++) {
    if (codes[i] == code) {
      return true;
    }
  }
  return false;
}

const char* shiftedSymbol(uint8_t code) {
  switch (code) {
    case 0x1E: return "!";
    case 0x1F: return "@";
    case 0x20: return "#";
    case 0x21: return "$";
    case 0x22: return "%";
    case 0x23: return "^";
    case 0x24: return "&";
    case 0x25: return "*";
    case 0x26: return "(";
    case 0x27: return ")";
    case 0x2D: return "_";
    case 0x2E: return "+";
    case 0x2F: return "{";
    case 0x30: return "}";
    case 0x31: return "|";
    case 0x33: return ":";
    case 0x34: return "\"";
    case 0x35: return "~";
    case 0x36: return "<";
    case 0x37: return ">";
    case 0x38: return "?";
    default: return nullptr;
  }
}

const char* keyName(uint8_t code, char* scratch, size_t scratchCap) {
  if (code >= 0x04 && code <= 0x1D) {
    scratch[0] = (char)('a' + (code - 0x04));
    scratch[1] = '\0';
    return scratch;
  }
  if (code >= 0x1E && code <= 0x26) {
    scratch[0] = (char)('1' + (code - 0x1E));
    scratch[1] = '\0';
    return scratch;
  }
  if (code == 0x27) {
    return "0";
  }
  switch (code) {
    case 0x01: return "ErrorRollOver";
    case 0x28: return "Enter";
    case 0x29: return "Esc";
    case 0x2A: return "Backspace";
    case 0x2B: return "Tab";
    case 0x2C: return "Space";
    case 0x2D: return "-";
    case 0x2E: return "=";
    case 0x2F: return "[";
    case 0x30: return "]";
    case 0x31: return "\\";
    case 0x33: return ";";
    case 0x34: return "'";
    case 0x35: return "`";
    case 0x36: return ",";
    case 0x37: return ".";
    case 0x38: return "/";
    case 0x39: return "CapsLock";
    case 0x3A: return "F1";
    case 0x3B: return "F2";
    case 0x3C: return "F3";
    case 0x3D: return "F4";
    case 0x3E: return "F5";
    case 0x3F: return "F6";
    case 0x40: return "F7";
    case 0x41: return "F8";
    case 0x42: return "F9";
    case 0x43: return "F10";
    case 0x44: return "F11";
    case 0x45: return "F12";
    case 0x46: return "PrintScreen";
    case 0x47: return "ScrollLock";
    case 0x48: return "Pause";
    case 0x49: return "Insert";
    case 0x4A: return "Home";
    case 0x4B: return "PageUp";
    case 0x4C: return "Delete";
    case 0x4D: return "End";
    case 0x4E: return "PageDown";
    case 0x4F: return "Right";
    case 0x50: return "Left";
    case 0x51: return "Down";
    case 0x52: return "Up";
    case 0x53: return "NumLock";
    case 0x54: return "KpSlash";
    case 0x55: return "KpStar";
    case 0x56: return "KpMinus";
    case 0x57: return "KpPlus";
    case 0x58: return "KpEnter";
    case 0x59: return "Kp1";
    case 0x5A: return "Kp2";
    case 0x5B: return "Kp3";
    case 0x5C: return "Kp4";
    case 0x5D: return "Kp5";
    case 0x5E: return "Kp6";
    case 0x5F: return "Kp7";
    case 0x60: return "Kp8";
    case 0x61: return "Kp9";
    case 0x62: return "Kp0";
    case 0x63: return "KpDot";
    default:
      snprintf(scratch, scratchCap, "0x%02X", code);
      return scratch;
  }
}

const char* mediaName(uint16_t usage) {
  switch (usage) {
    case 0x0030: return "Power";
    case 0x00B5: return "Next";
    case 0x00B6: return "Previous";
    case 0x00B7: return "Stop";
    case 0x00CD: return "PlayPause";
    case 0x00E2: return "Mute";
    case 0x00E9: return "VolumeUp";
    case 0x00EA: return "VolumeDown";
    case 0x0183: return "MediaSelect";
    case 0x0192: return "Calculator";
    case 0x0194: return "FileBrowser";
    case 0x0221: return "Search";
    case 0x0223: return "Home";
    case 0x0224: return "Back";
    case 0x0225: return "Forward";
    case 0x022A: return "Bookmarks";
    default: return nullptr;
  }
}

void appendMods(char* buf, size_t cap, size_t& used, uint8_t mod, bool includeShift) {
  const uint8_t mods[] = {0x01, 0x04, 0x08, 0x10, 0x40, 0x80};
  const char* modNames[] = {"LCTRL", "LALT", "LGUI", "RCTRL", "RALT", "RGUI"};
  for (int i = 0; i < 6; i++) {
    if (mod & mods[i]) {
      appendPiece(buf, cap, used, modNames[i], true);
    }
  }
  if (includeShift && (mod & 0x02) != 0) {
    appendPiece(buf, cap, used, "LSHIFT", true);
  }
  if (includeShift && (mod & 0x20) != 0) {
    appendPiece(buf, cap, used, "RSHIFT", true);
  }
}

void appendHex(char* buf, size_t cap, size_t& used, const uint8_t* data, size_t length) {
  for (size_t i = 0; i < length && used + 4 < cap; i++) {
    int wrote = snprintf(buf + used, cap - used, "%s%02X", (i == 0) ? "" : " ", data[i]);
    if (wrote > 0) {
      used += (size_t)wrote;
    }
  }
}

int16_t readI16(const uint8_t* p) {
  int16_t value;
  memcpy(&value, p, sizeof(value));
  return value;
}

void emitKeyboard(int slotIndex, const uint8_t* payload, size_t length) {
  Slot& slot = gSlots[slotIndex];
  const uint8_t* keys = payload;
  size_t keyLen = length;
  // Report protocol often leaves the report ID in front: ID, modifiers, reserved, keys.
  if (keyLen == 9 && keys[2] == 0) {
    keys++;
    keyLen--;
  }

  // A normal report is 8 bytes: modifier, reserved, six keys.
  // This keyboard sends 7: the report ID was removed, so the last key slot
  // is missing. Control then looked like "00 00 00 00 00 00 00".
  uint8_t boot[8];
  memset(boot, 0, sizeof(boot));
  if (keyLen == 8) {
    memcpy(boot, keys, 8);
  } else if (keyLen >= 1 && keyLen < 8) {
    boot[0] = keys[0];
    if (keyLen >= 2 && keys[1] == 0) {
      size_t count = keyLen - 2;
      if (count > 6) {
        count = 6;
      }
      memcpy(boot + 2, keys + 2, count);
    } else if (keyLen >= 2) {
      size_t count = keyLen - 1;
      if (count > 6) {
        count = 6;
      }
      memcpy(boot + 2, keys + 1, count);
    }
  } else {
    char line[160];
    size_t used = (size_t)snprintf(line, sizeof(line), "KBD  %s [%s] ", slot.name, slot.addr);
    appendHex(line, sizeof(line), used, payload, length);
    enqueueLine(line);
    pulseActivity();
    return;
  }

  uint8_t mod = boot[0];
  const uint8_t* codes = boot + 2;
  bool same = slot.haveLastKey && slot.lastMod == mod && memcmp(slot.lastKeys, codes, 6) == 0;
  if (same) {
    return;
  }
  bool ledsChanged = false;
  if (codeInKeys(codes, 0x53) && !codeInKeys(slot.lastKeys, 0x53)) {
    slot.numLock = !slot.numLock;
    ledsChanged = true;
  }
  if (codeInKeys(codes, 0x39) && !codeInKeys(slot.lastKeys, 0x39)) {
    slot.capsLock = !slot.capsLock;
    ledsChanged = true;
  }
  if (codeInKeys(codes, 0x47) && !codeInKeys(slot.lastKeys, 0x47)) {
    slot.scrollLock = !slot.scrollLock;
    ledsChanged = true;
  }
  if (ledsChanged) {
    slot.ledDirty = true;
  }
  slot.haveLastKey = true;
  slot.lastMod = mod;
  memcpy(slot.lastKeys, codes, 6);

  if (kHexPin >= 0 && digitalRead(kHexPin) == LOW) {
    char body[48];
    size_t used = 0;
    body[0] = '\0';
    for (int bit = 0; bit < 8; bit++) {
      if ((mod & (1 << bit)) == 0) {
        continue;
      }
      int wrote = snprintf(body + used, sizeof(body) - used, "%s%02X", used == 0 ? "" : " ", 0xE0 + bit);
      if (wrote > 0 && used + (size_t)wrote < sizeof(body)) {
        used += (size_t)wrote;
      }
    }
    for (int i = 0; i < 6; i++) {
      if (codes[i] == 0) {
        continue;
      }
      int wrote = snprintf(body + used, sizeof(body) - used, "%s%02X", used == 0 ? "" : " ", codes[i]);
      if (wrote > 0 && used + (size_t)wrote < sizeof(body)) {
        used += (size_t)wrote;
      }
    }
    if (used == 0) {
      return;
    }
    char line[160];
    snprintf(line, sizeof(line), "KBD  %s [%s] %s", slot.name, slot.addr, body);
    enqueueLine(line);
    pulseActivity();
    return;
  }

  bool shift = (mod & 0x22) != 0;
  bool letterUp = shift != slot.capsLock;
  char keysText[64];
  size_t keyUsed = 0;
  keysText[0] = '\0';
  bool anyKey = false;
  bool shiftLeftover = false;

  bool rollover = codes[0] == 0x01;
  for (int i = 1; i < 6; i++) {
    if (codes[i] != 0x01) {
      rollover = false;
    }
  }
  if (rollover) {
    appendPiece(keysText, sizeof(keysText), keyUsed, "ErrorRollOver", true);
    anyKey = true;
    shiftLeftover = shift;
  } else {
    for (int i = 0; i < 6; i++) {
      uint8_t code = codes[i];
      if (code == 0 || code == 0x39) {
        continue;
      }
      anyKey = true;
      char scratch[8];
      const char* name;
      if (code >= 0x04 && code <= 0x1D) {
        scratch[0] = (char)((letterUp ? 'A' : 'a') + (code - 0x04));
        scratch[1] = '\0';
        name = scratch;
      } else {
        const char* symbol = shift ? shiftedSymbol(code) : nullptr;
        if (symbol != nullptr) {
          name = symbol;
        } else {
          name = keyName(code, scratch, sizeof(scratch));
          if (shift) {
            shiftLeftover = true;
          }
        }
      }
      appendPiece(keysText, sizeof(keysText), keyUsed, name, true);
    }
  }

  bool otherMod = (mod & ~0x22) != 0;
  char body[96];
  size_t used = 0;
  body[0] = '\0';
  if (!anyKey && mod != 0 && !otherMod) {
    return;
  }
  if (!anyKey && !otherMod) {
    strncpy(body, "null", sizeof(body) - 1);
    body[sizeof(body) - 1] = '\0';
  } else {
    appendMods(body, sizeof(body), used, mod, shiftLeftover || !anyKey);
    if (anyKey) {
      appendPiece(body, sizeof(body), used, keysText, true);
    }
  }

  char line[160];
  snprintf(line, sizeof(line), "KBD  %s [%s] %s", slot.name, slot.addr, body);
  enqueueLine(line);
  if (anyKey || otherMod) {
    pulseActivity();
  }
}

void emitMouse(int slotIndex, const uint8_t* p, size_t n) {
  if (n < 3) {
    char line[160];
    size_t used = (size_t)snprintf(line, sizeof(line), "MOUSE %s [%s] ", gSlots[slotIndex].name, gSlots[slotIndex].addr);
    appendHex(line, sizeof(line), used, p, n);
    enqueueLine(line);
    return;
  }

  uint8_t buttons = p[0] & 0x1F;
  int x = 0;
  int y = 0;
  int wheel = 0;
  int pan = 0;
  bool hasWheel = false;
  bool hasPan = false;

  if (n <= 5) {
    x = (int8_t)p[1];
    y = (int8_t)p[2];
    if (n >= 4) {
      wheel = (int8_t)p[3];
      hasWheel = true;
    }
    if (n >= 5) {
      pan = (int8_t)p[4];
      hasPan = true;
    }
  } else if (n == 6 || n == 7) {
    x = readI16(p + 1);
    y = readI16(p + 3);
    wheel = (int8_t)p[5];
    hasWheel = true;
    if (n == 7) {
      pan = (int8_t)p[6];
      hasPan = true;
    }
  } else {
    x = readI16(p + 1);
    y = readI16(p + 3);
    if (n >= 7) {
      wheel = readI16(p + 5);
      hasWheel = true;
    }
    if (n >= 9) {
      pan = readI16(p + 7);
      hasPan = true;
    }
  }

  Slot& slot = gSlots[slotIndex];
  slot.accX += x;
  slot.accY += y;
  if (hasWheel) {
    slot.accWheel += wheel;
  }
  if (hasPan) {
    slot.accPan += pan;
  }

  bool buttonChanged = !slot.haveLastMouse || buttons != slot.lastButtons;
  uint32_t now = millis();
  bool due = (now - slot.lastMoveMs) >= 40;
  bool extra = slot.accWheel != 0 || slot.accPan != 0;
  if (!slot.haveLastMouse && buttons == 0 && slot.accX == 0 && slot.accY == 0 && !extra) {
    slot.haveLastMouse = true;
    slot.lastButtons = 0;
    return;
  }
  if (!buttonChanged && !due && !extra) {
    return;
  }
  if (!buttonChanged && slot.accX == 0 && slot.accY == 0 && slot.accWheel == 0 && slot.accPan == 0) {
    return;
  }

  char buttonsText[24];
  size_t buttonUsed = 0;
  buttonsText[0] = '\0';
  if (buttons == 0) {
    strncpy(buttonsText, "-", sizeof(buttonsText) - 1);
  } else {
    const char* names[] = {"L", "R", "M", "Back", "Fwd"};
    for (int bit = 0; bit < 5; bit++) {
      if (buttons & (1 << bit)) {
        appendPiece(buttonsText, sizeof(buttonsText), buttonUsed, names[bit], true);
      }
    }
  }

  bool moved = slot.accX != 0 || slot.accY != 0;
  char line[160];
  snprintf(line, sizeof(line), "MOUSE %s [%s] %s x=%ld y=%ld wheel=%ld pan=%ld",
           slot.name, slot.addr, buttonsText, (long)slot.accX, (long)slot.accY, (long)slot.accWheel,
           (long)slot.accPan);
  enqueueLine(line);
  if (moved) {
    pulseActivity();
  }

  slot.haveLastMouse = true;
  slot.lastButtons = buttons;
  slot.accX = 0;
  slot.accY = 0;
  slot.accWheel = 0;
  slot.accPan = 0;
  slot.lastMoveMs = now;
}

void emitConsumer(int slotIndex, const uint8_t* p, size_t n) {
  uint16_t usage = 0;
  if (n >= 2) {
    usage = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
  } else if (n == 1) {
    usage = p[0];
  }
  Slot& slot = gSlots[slotIndex];
  if (slot.haveLastMedia && slot.lastMedia == usage) {
    return;
  }
  slot.haveLastMedia = true;
  slot.lastMedia = usage;

  char line[160];
  if (usage == 0) {
    snprintf(line, sizeof(line), "MEDIA %s [%s] null", slot.name, slot.addr);
  } else {
    const char* name = mediaName(usage);
    if (name != nullptr) {
      snprintf(line, sizeof(line), "MEDIA %s [%s] %s", slot.name, slot.addr, name);
    } else {
      snprintf(line, sizeof(line), "MEDIA %s [%s] usage=0x%04X", slot.name, slot.addr, usage);
    }
  }
  enqueueLine(line);
}

void emitRaw(int slotIndex, const uint8_t* p, size_t n) {
  char line[160];
  size_t used = (size_t)snprintf(line, sizeof(line), "RAW  %s [%s] ", gSlots[slotIndex].name, gSlots[slotIndex].addr);
  appendHex(line, sizeof(line), used, p, n);
  enqueueLine(line);
}

void handleReport(NimBLEClient* client, uint16_t handle, const uint8_t* data, size_t length) {
  int slotIndex = findSlotByClient(client);
  if (slotIndex < 0 || data == nullptr || length == 0) {
    return;
  }
  int reportIndex = findReport(slotIndex, handle);
  uint8_t kind = kKindUnknown;
  const uint8_t* payload = data;
  size_t payloadLen = length;
  if (reportIndex >= 0) {
    const ReportMeta& report = gSlots[slotIndex].reports[reportIndex];
    kind = report.kind;
    if (report.idInPayload && report.reportId != 0 && length > 1 && data[0] == report.reportId) {
      payload++;
      payloadLen--;
    }
  }

  const Slot& slot = gSlots[slotIndex];
  if (kind == kKindUnknown) {
    if (slot.hasKeyboard && !slot.hasMouse && (payloadLen == 8 || payloadLen == 9)) {
      kind = kKindKeyboard;
    } else if (slot.hasMouse && !slot.hasKeyboard && payloadLen >= 3 && payloadLen <= 9) {
      kind = kKindMouse;
    }
  }

  if (kind == kKindKeyboard) {
    if (payloadLen > 9) {
      char line[160];
      size_t used = (size_t)snprintf(line, sizeof(line), "KBD  %s [%s] NKRO ", slot.name, slot.addr);
      appendHex(line, sizeof(line), used, payload, payloadLen);
      enqueueLine(line);
      return;
    }
    emitKeyboard(slotIndex, payload, payloadLen);
  } else if (kind == kKindMouse) {
    emitMouse(slotIndex, payload, payloadLen);
  } else if (kind == kKindConsumer) {
    emitConsumer(slotIndex, payload, payloadLen);
  } else {
    emitRaw(slotIndex, data, length);
  }
}

void notifyCB(NimBLERemoteCharacteristic* chr, uint8_t* data, size_t length, bool isNotify) {
  (void)isNotify;
  if (chr == nullptr) {
    return;
  }
  handleReport(chr->getClient(), chr->getHandle(), data, length);
}

// ---- scan and connect -------------------------------------------------------

bool shouldTake(const NimBLEAdvertisedDevice* device) {
  if (device == nullptr || !device->isConnectable()) {
    return false;
  }
  if (findFreeSlot() < 0 || !stillNeed()) {
    return false;
  }

  std::string name = device->getName();
  std::string addr = device->getAddress().toString();
  if (sameAddrText(addr.c_str())) {
    return false;
  }
  NimBLEClient* already = NimBLEDevice::getClientByPeerAddress(device->getAddress());
  if (already != nullptr && already->isConnected()) {
    return false;
  }
  bool bonded = NimBLEDevice::isBonded(device->getAddress());
  if (!bonded && !gPairNew) {
    return false;
  }

  uint16_t appearance = device->haveAppearance() ? device->getAppearance() : 0;
  bool service = device->isAdvertisingService(NimBLEUUID((uint16_t)kHidService));
  bool appearanceHid = appearance >= 0x03C0 && appearance <= 0x03CA;
  bool nameKeyboard = containsText(name, "keyboard") || containsText(name, "kbd") || containsText(name, "keypad");
  bool nameMouse = containsText(name, "mouse") || containsText(name, "trackball") || containsText(name, "trackpad");
  bool filterKeyboard = kKeyboardName[0] != '\0' && nameAllowed(name, kKeyboardName);
  bool filterMouse = kMouseName[0] != '\0' && nameAllowed(name, kMouseName);
  if (!bonded && !service && !appearanceHid && !nameKeyboard && !nameMouse && !filterKeyboard && !filterMouse) {
    return false;
  }

  int remembered = findMemory(addr.c_str());
  if (remembered >= 0 && gMemory[remembered].role == 4) {
    return false;
  }

  bool looksKeyboard = appearance == kAppearanceKeyboard || nameKeyboard || filterKeyboard;
  bool looksMouse = appearance == kAppearanceMouse || nameMouse || filterMouse;
  if (remembered >= 0) {
    if (gMemory[remembered].role == 1) {
      looksKeyboard = true;
      looksMouse = false;
    } else if (gMemory[remembered].role == 2) {
      looksMouse = true;
      looksKeyboard = false;
    } else if (gMemory[remembered].role == 3) {
      looksKeyboard = true;
      looksMouse = true;
    }
  }

  bool take = false;
  if (looksKeyboard && !looksMouse) {
    take = !haveKeyboard() && nameAllowed(name, kKeyboardName);
  } else if (looksMouse && !looksKeyboard) {
    take = !haveMouse() && nameAllowed(name, kMouseName);
  } else if (looksKeyboard && looksMouse) {
    // A combo may be the only device. Do not add it beside one we already have.
    if (!haveKeyboard() && !haveMouse()) {
      take = nameAllowed(name, kKeyboardName) || nameAllowed(name, kMouseName);
    }
  } else {
    bool keyboardOk = !haveKeyboard() && (kKeyboardName[0] == '\0' || nameAllowed(name, kKeyboardName));
    bool mouseOk = !haveMouse() && (kMouseName[0] == '\0' || nameAllowed(name, kMouseName));
    take = keyboardOk || mouseOk;
  }

  const char* shown = name.empty() ? "(no name)" : name.c_str();
  enqueuef("Found %s [%s] appearance=0x%04X%s", shown, addr.c_str(), appearance, take ? "" : " (skip)");
  return take;
}

void configureClient(NimBLEClient* client, bool shareRadio) {
  client->setClientCallbacks(&gClientCallbacks, false);
  if (shareRadio) {
    // 30-50 ms. Leaves airtime for the other link and for scanning.
    client->setConnectionParams(24, 40, 0, 500);
  } else {
    // 15-30 ms while this is the only link.
    client->setConnectionParams(12, 24, 0, 400);
  }
  client->setConnectTimeout(8000);
}

void relaxLinksForScan() {
  for (int i = 0; i < kSlotCount; i++) {
    NimBLEClient* client = gSlots[i].client;
    if (!gSlots[i].used || client == nullptr || !client->isConnected()) {
      continue;
    }
    client->setConnectionParams(24, 40, 0, 500);
    client->updateConnParams(24, 40, 0, 500);
  }
}

void noteScanTarget() {
  int note = 0;
  if (!haveKeyboard()) {
    note |= 1;
  }
  if (!haveMouse()) {
    note |= 2;
  }
  if (note == gScanNote) {
    return;
  }
  gScanNote = note;
  if (note == 3) {
    logf("Scanning for a BLE keyboard and mouse...");
  } else if (note == 1) {
    logf("Scanning for the keyboard...");
  } else if (note == 2) {
    logf("Scanning for the mouse...");
  }
}

void startScan() {
  if (!gAllowScan || !stillNeed() || gConnecting || gPending != nullptr) {
    return;
  }
  NimBLEScan* scan = NimBLEDevice::getScan();
  if (scan->isScanning()) {
    gScanState = kScanRunning;
    return;
  }
  if (gScanState != kScanIdle) {
    gWantScan = true;
    return;
  }
  uint32_t now = millis();
  if ((int32_t)(now - gNextScanMs) < 0) {
    gWantScan = true;
    return;
  }

  int links = connectedCount();
  if (links > 0) {
    // Short window so the link that is already up keeps its connection events.
    scan->setInterval(160);
    scan->setWindow(40);
  } else {
    scan->setInterval(100);
    scan->setWindow(60);
  }

  gScanState = kScanRunning;
  gScanStateMs = now;
  noteScanTarget();
  // A few seconds, then stop and start again. That clears the duplicate
  // filter, and a failed start can be retried. Duration 0 never came back
  // once the first device was connected.
  uint32_t duration = links > 0 ? 2500 : 5000;
  if (!scan->start(duration, false, false)) {
    gScanState = kScanIdle;
    gNextScanMs = now + 600;
    gWantScan = true;
    return;
  }
}

uint8_t roleOfSlot(int slotIndex) {
  const Slot& slot = gSlots[slotIndex];
  if (slot.hasKeyboard && slot.hasMouse) {
    return 3;
  }
  if (slot.hasKeyboard) {
    return 1;
  }
  if (slot.hasMouse) {
    return 2;
  }
  return 4;
}

bool connectToServer(const NimBLEAdvertisedDevice* device) {
  std::string advAddr = device->getAddress().toString();
  std::string advName = device->getName();
  logf("Connecting to %s [%s]", advName.empty() ? "(no name)" : advName.c_str(), advAddr.c_str());

  NimBLEClient* client = nullptr;
  if (NimBLEDevice::getCreatedClientCount() > 0) {
    client = NimBLEDevice::getClientByPeerAddress(device->getAddress());
    if (client == nullptr) {
      client = NimBLEDevice::getDisconnectedClient();
    }
  }

  bool shareRadio = connectedCount() > 0;
  if (client == nullptr) {
    if (NimBLEDevice::getCreatedClientCount() >= MYNEWT_VAL(BLE_MAX_CONNECTIONS)) {
      static uint32_t lastFullLog = 0;
      if (millis() - lastFullLog > 10000) {
        lastFullLog = millis();
        logf("No free BLE connection (%d in use, %d allowed).", connectedCount(), MYNEWT_VAL(BLE_MAX_CONNECTIONS));
        if (MYNEWT_VAL(BLE_MAX_CONNECTIONS) < 2) {
          logf("This NimBLE build allows one connection. A keyboard and a mouse need two.");
        }
      }
      return false;
    }
    client = NimBLEDevice::createClient();
    if (client == nullptr) {
      logf("Could not create a BLE client.");
      return false;
    }
    configureClient(client, shareRadio);
    if (!client->connect(device)) {
      NimBLEDevice::deleteClient(client);
      logf("Connect failed.");
      return false;
    }
  } else if (!client->isConnected()) {
    configureClient(client, shareRadio);
    if (!client->connect(device)) {
      logf("Reconnect failed.");
      return false;
    }
  }

  if (!client->isConnected()) {
    logf("Link dropped during connect.");
    return false;
  }

  int slotIndex = findFreeSlot();
  if (slotIndex < 0) {
    client->disconnect();
    logf("Both device slots are already in use.");
    return false;
  }

  Slot& slot = gSlots[slotIndex];
  clearSlot(slotIndex);
  slot.used = true;
  slot.client = client;
  copyText(slot.name, sizeof(slot.name), advName, "HID");
  strncpy(slot.advert, advAddr.c_str(), sizeof(slot.advert) - 1);
  slot.advert[sizeof(slot.advert) - 1] = '\0';
  std::string peer = client->getPeerAddress().toString();
  copyText(slot.addr, sizeof(slot.addr), peer, advAddr.c_str());
  slot.appearance = device->haveAppearance() ? device->getAppearance() : 0;

  bool bonded = NimBLEDevice::isBonded(device->getAddress()) || NimBLEDevice::isBonded(client->getPeerAddress());
  if (!bonded && !gPairNew) {
    logf("%s is not paired. Press BOOT to pair a new device.", slot.name);
    client->disconnect();
    NimBLEDevice::deleteClient(client);
    clearSlot(slotIndex);
    return false;
  }
  if (bonded) {
    logf("Connected to %s [%s]. Using the saved bond.", slot.name, slot.addr);
  } else {
    logf("Connected to %s [%s]. Pairing as a new device.", slot.name, slot.addr);
  }
  if (!client->secureConnection()) {
    logf("Pairing did not finish. Reports that require encryption will not arrive.");
    logf("If this repeats: put the device in pairing mode, or type forget.");
  }
  drainLines();
  if (!client->isConnected()) {
    clearSlot(slotIndex);
    logf("Disconnected during pairing.");
    return false;
  }

  NimBLERemoteService* service = client->getService(NimBLEUUID((uint16_t)kHidService));
  if (service == nullptr) {
    logf("No HID service on this device. Skipping it.");
    rememberDevice(advAddr.c_str(), 4);
    rememberDevice(slot.addr, 4);
    client->disconnect();
    NimBLEDevice::deleteClient(client);
    return false;
  }

  NimBLERemoteCharacteristic* mapChr = service->getCharacteristic(NimBLEUUID((uint16_t)kReportMap));
  if (mapChr != nullptr && mapChr->canRead()) {
    NimBLEAttValue map = mapChr->readValue();
    if (map.size() > 0) {
      parseReportMap(slotIndex, map.data(), map.size());
      logf("Read HID report map (%u bytes).", (unsigned)map.size());
    }
  }

  NimBLERemoteCharacteristic* list[16];
  size_t listCount = 0;
  const std::vector<NimBLERemoteCharacteristic*>& chars = service->getCharacteristics(true);
  for (size_t i = 0; i < chars.size() && listCount < 16; i++) {
    list[listCount++] = chars[i];
  }

  int reportChars = 0;
  for (size_t i = 0; i < listCount; i++) {
    if (list[i]->getUUID() == NimBLEUUID((uint16_t)kHidReport)) {
      reportChars++;
    }
  }

  // Report mode when the device has HID reports. Boot mode only when it does not.
  // Control point 0x01 is Exit Suspend, which lets the device send input.
  NimBLERemoteCharacteristic* protocol = service->getCharacteristic(NimBLEUUID((uint16_t)kProtocolMode));
  writeByte(protocol, reportChars > 0 ? 0x01 : 0x00);
  writeByte(service->getCharacteristic(NimBLEUUID((uint16_t)kHidControlPoint)), 0x01);

  bool needKeyboard = true;
  bool needMouse = true;
  for (int i = 0; i < kSlotCount; i++) {
    if (i == slotIndex || !gSlots[i].used) {
      continue;
    }
    if (gSlots[i].hasKeyboard) {
      needKeyboard = false;
    }
    if (gSlots[i].hasMouse) {
      needMouse = false;
    }
  }

  for (size_t i = 0; i < listCount; i++) {
    NimBLERemoteCharacteristic* chr = list[i];
    if (!(chr->getUUID() == NimBLEUUID((uint16_t)kHidReport)) || !canWriteReport(chr)) {
      continue;
    }
    uint8_t reportId = 0;
    uint8_t reportType = 1;
    NimBLERemoteDescriptor* ref = chr->getDescriptor(NimBLEUUID((uint16_t)kReportRef));
    if (ref != nullptr) {
      NimBLEAttValue value = ref->readValue();
      if (value.size() >= 2) {
        reportId = value[0];
        reportType = value[1];
      }
    }
    if (reportType != 2) {
      continue;
    }
    bool matchesLed = slot.haveLedId && reportId == slot.ledMapId;
    bool matchesKeyboard = kindForId(slotIndex, reportId) == kKindKeyboard;
    bool take = matchesLed || matchesKeyboard;
    if (!take && slot.ledOutput == nullptr && !slot.haveLedId && slot.idCount == 0 &&
        slot.appearance == kAppearanceKeyboard) {
      take = true;
    }
    if (!take) {
      continue;
    }
    slot.ledOutput = chr;
  }
  if (needKeyboard && slot.ledOutput == nullptr) {
    NimBLERemoteCharacteristic* only = nullptr;
    int outputs = 0;
    for (size_t i = 0; i < listCount; i++) {
      NimBLERemoteCharacteristic* chr = list[i];
      if (!(chr->getUUID() == NimBLEUUID((uint16_t)kHidReport)) || !canWriteReport(chr)) {
        continue;
      }
      uint8_t reportType = 1;
      NimBLERemoteDescriptor* ref = chr->getDescriptor(NimBLEUUID((uint16_t)kReportRef));
      if (ref != nullptr) {
        NimBLEAttValue value = ref->readValue();
        if (value.size() >= 2) {
          reportType = value[1];
        }
      }
      if (reportType != 2) {
        continue;
      }
      outputs++;
      only = chr;
    }
    if (outputs == 1) {
      slot.ledOutput = only;
    }
  }
  if (needKeyboard) {
    NimBLERemoteCharacteristic* bootOut = service->getCharacteristic(NimBLEUUID((uint16_t)kBootKeyboardOutput));
    if (canWriteReport(bootOut)) {
      slot.bootLed = bootOut;
    }
    if (slot.ledOutput != nullptr || slot.bootLed != nullptr) {
      slot.numLock = false;
      slot.capsLock = false;
      slot.scrollLock = false;
      slot.ledDirty = true;
      logf("Keyboard lock LEDs enabled.");
    }
  }

  int subscribed = 0;
  bool gotKeyboard = false;
  bool gotMouse = false;
  for (size_t i = 0; i < listCount; i++) {
    NimBLERemoteCharacteristic* chr = list[i];
    if (!(chr->getUUID() == NimBLEUUID((uint16_t)kHidReport))) {
      continue;
    }
    uint8_t reportId = 0;
    uint8_t reportType = 1;
    NimBLERemoteDescriptor* ref = chr->getDescriptor(NimBLEUUID((uint16_t)kReportRef));
    if (ref != nullptr) {
      NimBLEAttValue value = ref->readValue();
      if (value.size() >= 2) {
        reportId = value[0];
        reportType = value[1];
      }
    }
    if (reportType != 1) {
      continue;  // 2 = output (keyboard LEDs), 3 = feature
    }
    uint8_t kind = kindForId(slotIndex, reportId);
    if (kind == kKindKeyboard && !needKeyboard) {
      logf("Skipped keyboard report id %u. A keyboard is already connected.", reportId);
      continue;
    }
    if (kind == kKindMouse && !needMouse) {
      logf("Skipped mouse report id %u. A mouse is already connected.", reportId);
      continue;
    }
    addReport(slotIndex, chr->getHandle(), reportId, kind, slot.mapUsesReportIds);
    if (kind == kKindKeyboard) {
      gotKeyboard = true;
    } else if (kind == kKindMouse) {
      gotMouse = true;
    }
    if (subscribeInput(chr)) {
      subscribed++;
      logf("Subscribed report id %u (%s).", reportId,
           kind == kKindKeyboard ? "keyboard" : kind == kKindMouse ? "mouse" : kind == kKindConsumer ? "media" : "hid");
    } else {
      logf("Could not subscribe report id %u.", reportId);
    }
  }

  if (!gotKeyboard && needKeyboard) {
    NimBLERemoteCharacteristic* boot = service->getCharacteristic(NimBLEUUID((uint16_t)kBootKeyboard));
    if (boot != nullptr) {
      addReport(slotIndex, boot->getHandle(), 0, kKindKeyboard, false);
      if (subscribeInput(boot)) {
        subscribed++;
        gotKeyboard = true;
        logf("Subscribed boot keyboard.");
      }
    }
  }
  if (!gotMouse && needMouse) {
    NimBLERemoteCharacteristic* boot = service->getCharacteristic(NimBLEUUID((uint16_t)kBootMouse));
    if (boot != nullptr) {
      addReport(slotIndex, boot->getHandle(), 0, kKindMouse, false);
      if (subscribeInput(boot)) {
        subscribed++;
        gotMouse = true;
        logf("Subscribed boot mouse.");
      }
    }
  }

  if (!slot.hasKeyboard && !slot.hasMouse) {
    if (needKeyboard && slot.appearance == kAppearanceKeyboard) {
      slot.hasKeyboard = true;
    } else if (needMouse && slot.appearance == kAppearanceMouse) {
      slot.hasMouse = true;
    }
  }

  if (!client->isConnected()) {
    clearSlot(slotIndex);
    logf("Disconnected while subscribing.");
    return false;
  }

  if (subscribed == 0) {
    logf("No input reports could be enabled. Will try this device again.");
    client->disconnect();
    NimBLEDevice::deleteClient(client);
    return false;
  }

  bool useful = (slot.hasKeyboard && needKeyboard) || (slot.hasMouse && needMouse);
  uint8_t role = roleOfSlot(slotIndex);
  if (!useful) {
    if (role == 4) {
      logf("%s is not a keyboard or mouse. Ignoring it.", slot.name);
      rememberDevice(advAddr.c_str(), 4);
      rememberDevice(slot.addr, 4);
    } else {
      logf("%s does not fill a missing device. Leaving it for later.", slot.name);
      rememberDevice(advAddr.c_str(), role);
      rememberDevice(slot.addr, role);
    }
    client->disconnect();
    NimBLEDevice::deleteClient(client);
    return false;
  }

  rememberDevice(advAddr.c_str(), role);
  rememberDevice(slot.addr, role);
  if (slot.hasKeyboard && slot.hasMouse) {
    logf("Ready: %s is both a keyboard and a mouse.", slot.name);
    logf("Both roles are filled by this one device, so scanning stops.");
  } else if (slot.hasKeyboard) {
    logf("Keyboard ready: %s", slot.name);
  } else if (slot.hasMouse) {
    logf("Mouse ready: %s", slot.name);
  }
  if (slot.hasKeyboard && slot.ledOutput == nullptr && slot.bootLed == nullptr) {
    logf("This keyboard has no lock LED report. Caps Lock and Num Lock lights stay off.");
  }
  if (!stillNeed()) {
    gAllowScan = false;
    gPairNew = false;
  } else if (gAllowScan) {
    if (!haveKeyboard()) {
      logf("Keyboard is still missing.");
    }
    if (!haveMouse()) {
      logf("Mouse is still missing.");
    }
  }
  return true;
}

// ---- callbacks --------------------------------------------------------------

void ClientCallbacks::onConnect(NimBLEClient* pClient) {
  enqueuef("Link up [%s]", pClient->getPeerAddress().toString().c_str());
}

void ClientCallbacks::onConnectFail(NimBLEClient* pClient, int reason) {
  (void)pClient;
  enqueuef("Connect failed, reason=%d", reason);
}

void ClientCallbacks::onDisconnect(NimBLEClient* pClient, int reason) {
  char name[32] = "HID";
  char addr[18] = "";
  int slotIndex = findSlotByClient(pClient);
  if (slotIndex >= 0) {
    strncpy(name, gSlots[slotIndex].name, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    strncpy(addr, gSlots[slotIndex].addr, sizeof(addr) - 1);
    addr[sizeof(addr) - 1] = '\0';
    clearSlot(slotIndex);
  }
  enqueuef("Disconnected %s [%s] reason=%d", name, addr, reason);
  gScanNote = -1;
  gLinksRelaxed = false;
  if (!stillNeed()) {
    return;
  }
  // The saved device can come back. A short BOOT press is what allows a new one.
  if (NimBLEDevice::getNumBonds() > 0 || gPairNew) {
    gAllowScan = true;
    gWantScan = true;
    if (!gPairNew) {
      enqueueLine("Looking again for a paired keyboard or mouse.");
    }
  } else {
    enqueueLine("Press the BOOT button to pair a keyboard or mouse.");
  }
}

void ClientCallbacks::onPassKeyEntry(NimBLEConnInfo& connInfo) {
  uint32_t pin = gRuntimePasskey != 0 ? gRuntimePasskey : kPasskey;
  enqueuef("Device asked for a passkey. Sending %06lu.", (unsigned long)pin);
  NimBLEDevice::injectPassKey(connInfo, pin);
}

void ClientCallbacks::onConfirmPasskey(NimBLEConnInfo& connInfo, uint32_t pin) {
  enqueuef("Numeric comparison %06lu, accepted.", (unsigned long)pin);
  NimBLEDevice::injectConfirmPasskey(connInfo, true);
}

void ClientCallbacks::onAuthenticationComplete(NimBLEConnInfo& connInfo) {
  if (connInfo.isEncrypted()) {
    enqueuef("Link encrypted.");
  } else {
    enqueuef("Encryption failed. Type forget, then pair the device again.");
  }
}

void ScanCallbacks::onResult(const NimBLEAdvertisedDevice* advertisedDevice) {
  if (gConnecting || gPending != nullptr) {
    return;
  }
  if (!shouldTake(advertisedDevice)) {
    return;
  }
  gPending = advertisedDevice;
  gPendingMs = millis();
  gConnecting = true;
  NimBLEScan* scan = NimBLEDevice::getScan();
  if (scan->isScanning()) {
    gScanState = kScanStopping;
    gScanStateMs = millis();
    scan->stop();
  } else {
    gScanState = kScanIdle;
  }
}

void ScanCallbacks::onScanEnd(const NimBLEScanResults& results, int reason) {
  (void)results;
  (void)reason;
  // Completion for the scan we just cancelled can arrive after the next
  // scan has already started. Do not mark that newer scan idle.
  if (NimBLEDevice::getScan()->isScanning()) {
    gScanState = kScanRunning;
    return;
  }
  gScanState = kScanIdle;
  gScanStateMs = millis();
  if (gAllowScan && !gConnecting && gPending == nullptr && stillNeed()) {
    gNextScanMs = millis() + (connectedCount() > 0 ? 200 : 40);
    gWantScan = true;
  }
}

// ---- serial commands --------------------------------------------------------

bool commandIs(const char* cmd, const char* literal) {
  while (*literal) {
    char c = *cmd++;
    if (c >= 'A' && c <= 'Z') {
      c = (char)(c - 'A' + 'a');
    }
    if (c != *literal++) {
      return false;
    }
  }
  return *cmd == '\0';
}

void printStatus() {
  bool any = false;
  for (int i = 0; i < kSlotCount; i++) {
    if (!gSlots[i].used) {
      continue;
    }
    any = true;
    const char* kind = "HID";
    if (gSlots[i].hasKeyboard && gSlots[i].hasMouse) {
      kind = "keyboard+mouse";
    } else if (gSlots[i].hasKeyboard) {
      kind = "keyboard";
    } else if (gSlots[i].hasMouse) {
      kind = "mouse";
    }
    emitf("  %s  %s [%s]", kind, gSlots[i].name, gSlots[i].addr);
  }
  if (!any) {
    emitLine("  no devices connected");
  }
  if (!haveKeyboard()) {
    emitLine("  keyboard: still looking");
  }
  if (!haveMouse()) {
    emitLine("  mouse: still looking");
  }
  emitf("  bonds stored: %d", NimBLEDevice::getNumBonds());
  emitf("  connections: %d of %d", connectedCount(), MYNEWT_VAL(BLE_MAX_CONNECTIONS));
  emitf("  passkey mode: %s", kUsePasskey ? "on" : "Just Works");
}

void forgetBonds() {
  emitLine("Erasing bonds and restarting...");
  gWantScan = false;
  NimBLEScan* scan = NimBLEDevice::getScan();
  if (scan->isScanning()) {
    scan->stop();
  }
  std::vector<NimBLEClient*> clients = NimBLEDevice::getConnectedClients();
  for (size_t i = 0; i < clients.size(); i++) {
    clients[i]->disconnect();
  }
  delay(200);
  NimBLEDevice::deleteAllBonds();
  delay(100);
  ESP.restart();
}

void armScan(const char* why) {
  if (!gBleReady) {
    return;
  }
  if (!stillNeed()) {
    emitLine("A keyboard and a mouse are already connected.");
    emitLine("A second keyboard or a second mouse is not connected.");
    return;
  }
  gAllowScan = true;
  gScanNote = -1;
  gWantScan = true;
  if (!gConnecting && gPending == nullptr && gScanState == kScanRunning && NimBLEDevice::getScan()->isScanning()) {
    gScanState = kScanStopping;
    gScanStateMs = millis();
    NimBLEDevice::getScan()->stop();
  }
  emitLine(why);
  if (!haveKeyboard()) {
    emitLine("Looking for a keyboard.");
  }
  if (!haveMouse()) {
    emitLine("Looking for a mouse.");
  }
}

void handleCommand(char* cmd) {
  while (*cmd == ' ') {
    cmd++;
  }
  if (cmd[0] == '\0') {
    return;
  }
  if (commandIs(cmd, "status")) {
    printStatus();
    return;
  }
  if (commandIs(cmd, "help")) {
    emitLine("Commands: status, rescan, forget, pin 123456");
    emitLine("Short BOOT press pairs a new device. Hold BOOT 5 seconds to forget bonds.");
    return;
  }
  if (commandIs(cmd, "rescan")) {
    clearMemory();
    armScan("Scan filters cleared.");
    return;
  }
  if (commandIs(cmd, "forget")) {
    forgetBonds();
    return;
  }
  if ((cmd[0] == 'p' || cmd[0] == 'P') && (cmd[1] == 'i' || cmd[1] == 'I') && (cmd[2] == 'n' || cmd[2] == 'N') &&
      cmd[3] == ' ') {
    uint32_t pin = (uint32_t)strtoul(cmd + 4, nullptr, 10);
    if (pin > 999999) {
      emitLine("Passkey must be 0..999999.");
      return;
    }
    gRuntimePasskey = pin;
    NimBLEDevice::setSecurityPasskey(pin);
    emitf("Passkey set to %06lu.", (unsigned long)pin);
    if (!kUsePasskey) {
      emitLine("kUsePasskey is false, so Just Works is still the first choice.");
      emitLine("Set kUsePasskey to true and upload again if the device rejects that.");
    }
    return;
  }
  emitLine("Unknown command. Try: status, rescan, forget, pin 123456");
}

void pollStream(Stream& port, char* data, uint8_t* len, uint8_t cap) {
  while (port.available() > 0) {
    char c = (char)port.read();
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      data[*len] = '\0';
      handleCommand(data);
      *len = 0;
      continue;
    }
    if (*len < cap - 1) {
      data[(*len)++] = c;
    }
  }
}

void pollSerial() {
  pollStream(Serial, gUsbCmd, &gUsbCmdLen, sizeof(gUsbCmd));
  if (kLinkUart) {
    pollStream(Serial1, gLinkCmd, &gLinkCmdLen, sizeof(gLinkCmd));
  }
}

void pulseActivity() {
  if (kActivityPin < 0) {
    return;
  }
  digitalWrite(kActivityPin, HIGH);
  uint32_t until = millis() + kActivityPulseMs;
  if (until == 0) {
    until = 1;
  }
  gActivityUntil = until;
}

void serviceLinkLeds() {
  if (kKeyboardLedPin >= 0) {
    digitalWrite(kKeyboardLedPin, haveKeyboard() ? HIGH : LOW);
  }
  if (kMouseLedPin >= 0) {
    digitalWrite(kMouseLedPin, haveMouse() ? HIGH : LOW);
  }
}

void serviceActivity() {
  if (kActivityPin < 0) {
    return;
  }
  uint32_t until = gActivityUntil;
  if (until == 0) {
    return;
  }
  if ((int32_t)(millis() - until) < 0) {
    return;
  }
  if (gActivityUntil == until) {
    digitalWrite(kActivityPin, LOW);
    gActivityUntil = 0;
  }
}

void pollBoot() {
  if (kBootPin < 0) {
    return;
  }
  bool released = digitalRead(kBootPin) != LOW;
  uint32_t now = millis();
  static bool ready = false;
  static bool lastReleased = true;
  static bool pressed = false;
  static bool longHandled = false;
  static uint32_t changeMs = 0;
  if (released != lastReleased) {
    lastReleased = released;
    changeMs = now;
  }
  if ((uint32_t)(now - changeMs) < 40) {
    return;
  }
  if (!ready) {
    if (released) {
      ready = true;
    }
    return;
  }
  if (!released) {
    if (!pressed) {
      pressed = true;
      longHandled = false;
    }
    if (!longHandled && (uint32_t)(now - changeMs) >= kBootForgetMs) {
      longHandled = true;
      emitLine("BOOT held for 5 seconds.");
      forgetBonds();
    }
    return;
  }
  if (pressed) {
    pressed = false;
    if (!longHandled) {
      if (!stillNeed()) {
        emitLine("A keyboard and a mouse are already connected.");
        emitLine("Hold BOOT for 5 seconds to forget every paired device.");
        return;
      }
      gPairNew = true;
      clearMemory();
      armScan("BOOT pressed. New pairing is allowed.");
    }
  }
}

// ---- Arduino entry points ---------------------------------------------------

void setup() {
  Serial.begin(115200);
  if (kLinkUart) {
    Serial1.begin(kLinkBaud, SERIAL_8N1, kLinkRxPin, kLinkTxPin);
  }
  if (kBootPin >= 0) {
    pinMode(kBootPin, INPUT_PULLUP);
  }
  if (kActivityPin >= 0) {
    pinMode(kActivityPin, OUTPUT);
    digitalWrite(kActivityPin, LOW);
  }
  if (kHexPin >= 0) {
    pinMode(kHexPin, INPUT_PULLUP);
  }
  if (kKeyboardLedPin >= 0) {
    pinMode(kKeyboardLedPin, OUTPUT);
    digitalWrite(kKeyboardLedPin, LOW);
  }
  if (kMouseLedPin >= 0) {
    pinMode(kMouseLedPin, OUTPUT);
    digitalWrite(kMouseLedPin, LOW);
  }
  uint32_t start = millis();
  while (!Serial && (millis() - start) < 2500) {
    delay(10);
  }

  for (int i = 0; i < kSlotCount; i++) {
    clearSlot(i);
  }
  clearMemory();
  gLines = xQueueCreate(24, sizeof(LineEvent));

  emitLine("");
  emitLine("ESP32-C3 BLE HID host");
  emitLine("This board connects TO one BLE keyboard and one BLE mouse.");
  emitLine("A second keyboard or a second mouse is not connected.");
  emitLine("Classic Bluetooth devices are not supported on ESP32-C3.");
  emitLine("On boot this board reconnects only to devices it already paired.");
  emitLine("Short press BOOT to pair a new keyboard or mouse.");
  emitLine("Hold BOOT for 5 seconds to forget every paired device.");
  emitLine("Commands: status, rescan, forget, pin 123456");
  emitLine("Lines look like:");
  emitLine("  KBD   Name [addr] A");
  emitLine("  MOUSE Name [addr] L x=3 y=-1 wheel=0 pan=0");
  emitLine("  MEDIA Name [addr] VolumeUp");
  if (kLinkUart) {
    emitf("Link UART: TX GPIO%d, RX GPIO%d, %lu baud", kLinkTxPin, kLinkRxPin, (unsigned long)kLinkBaud);
  }
  if (kBootPin >= 0) {
    emitf("BOOT button: GPIO%d. Short press pairs. Hold %lu ms to forget.", kBootPin,
          (unsigned long)kBootForgetMs);
  }
  if (kActivityPin >= 0) {
    emitf("Activity pin: GPIO%d, high for %lu ms on a key press or mouse move", kActivityPin,
          (unsigned long)kActivityPulseMs);
  }
  if (kHexPin >= 0) {
    emitf("Hex pin: GPIO%d, pull-up on. Tie it to GND to print key codes in hex.", kHexPin);
  }
  if (kKeyboardLedPin >= 0) {
    emitf("Keyboard LED: GPIO%d, high while a keyboard is connected.", kKeyboardLedPin);
  }
  if (kMouseLedPin >= 0) {
    emitf("Mouse LED: GPIO%d, high while a mouse is connected.", kMouseLedPin);
  }

  if (!NimBLEDevice::init("ESP32C3-HID-Host")) {
    emitLine("BLE init failed.");
    return;
  }
  gBleReady = true;

  // Bond so the keyboard and mouse reconnect after a reset.
  // Just Works (no PIN) is what most BLE mice and many keyboards accept.
  if (kUsePasskey) {
    NimBLEDevice::setSecurityAuth(true, true, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_ONLY);
    NimBLEDevice::setSecurityPasskey(kPasskey);
    gRuntimePasskey = kPasskey;
  } else {
    NimBLEDevice::setSecurityAuth(true, false, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  }
  NimBLEDevice::setPower(9);
  emitf("BLE connections available: %d", MYNEWT_VAL(BLE_MAX_CONNECTIONS));
  if (MYNEWT_VAL(BLE_MAX_CONNECTIONS) < 2) {
    emitLine("This NimBLE build allows only one connection.");
    emitLine("A keyboard and a mouse need two. Install NimBLE-Arduino 2.x on ESP32 board package 3.x.");
  }

  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&gScanCallbacks, false);
  scan->setInterval(100);
  scan->setWindow(60);
  scan->setActiveScan(true);
  gPairNew = false;
  if (NimBLEDevice::getNumBonds() > 0) {
    emitf("Paired devices saved: %d. Connecting only to those.", NimBLEDevice::getNumBonds());
    gAllowScan = true;
    gWantScan = true;
  } else {
    emitLine("No paired keyboard or mouse. Press BOOT to pair one.");
    gAllowScan = false;
    gWantScan = false;
  }
}

void loop() {
  if (!gBleReady) {
    delay(1000);
    return;
  }
  drainLines();

  if (gScanState == kScanStopping && (millis() - gScanStateMs) > 1500) {
    if (!NimBLEDevice::getScan()->isScanning()) {
      gScanState = kScanIdle;
      if (gAllowScan && !gConnecting && gPending == nullptr && stillNeed()) {
        gWantScan = true;
      }
    }
  }

  bool scanIdle = gScanState == kScanIdle && !NimBLEDevice::getScan()->isScanning();
  bool pendingWaitOver = gPending != nullptr && (millis() - gPendingMs) > 1500;
  if (gPending != nullptr && (scanIdle || pendingWaitOver)) {
    const NimBLEAdvertisedDevice* device = gPending;
    gPending = nullptr;
    bool ok = connectToServer(device);
    gConnecting = false;
    if (ok && stillNeed() && connectedCount() > 0) {
      relaxLinksForScan();
      gLinksRelaxed = true;
      gNextScanMs = millis() + 300;
    }
    if (!ok) {
      gNextScanMs = millis() + 400;
    }
    if (!stillNeed()) {
      gAllowScan = false;
      gPairNew = false;
    }
    bool oneLinkLimit = MYNEWT_VAL(BLE_MAX_CONNECTIONS) < 2 && connectedCount() >= 1;
    gWantScan = gAllowScan && stillNeed() && !oneLinkLimit;
  }

  if (gWantScan && gPending == nullptr && !gConnecting && gScanState == kScanIdle &&
      (int32_t)(millis() - gNextScanMs) >= 0) {
    if (connectedCount() > 0 && !gLinksRelaxed) {
      relaxLinksForScan();
      gLinksRelaxed = true;
      gNextScanMs = millis() + 300;
    } else {
      gWantScan = false;
      startScan();
    }
  }

  if (!stillNeed()) {
    gAllowScan = false;
    gPairNew = false;
    if (!gReadyNoted) {
      gReadyNoted = true;
      logf("Receiving. Type on the keyboard or move the mouse.");
    }
  } else {
    gReadyNoted = false;
  }

  static uint32_t lastNote = 0;
  if (gAllowScan && stillNeed() && gScanState != kScanStopping && (millis() - lastNote) > 15000) {
    lastNote = millis();
    if (MYNEWT_VAL(BLE_MAX_CONNECTIONS) < 2 && connectedCount() >= 1) {
      logf("One device is connected. This NimBLE build allows only one connection.");
    } else if (!gPairNew) {
      logf("Still looking for a paired device. Press BOOT to pair a new one.");
    } else if (!haveKeyboard() && !haveMouse()) {
      logf("Still scanning. Put the keyboard and the mouse in pairing mode. They cannot stay connected to a PC.");
    } else if (!haveKeyboard()) {
      logf("Mouse is connected. Still looking for the keyboard.");
    } else {
      logf("Keyboard is connected. Still looking for the mouse.");
    }
  }

  serviceKeyboardLeds();
  serviceLinkLeds();
  serviceActivity();
  pollBoot();
  pollSerial();
  drainLines();
  delay(10);
}
