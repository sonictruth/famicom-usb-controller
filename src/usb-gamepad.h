// USB HID gamepad on the ESP32-S3's native USB (OTG) port via EspUsbHost,
// decoded into NES button bits. Pressed buttons are logged to Serial.
#pragma once
#include <stdint.h>

// NES button bits, in the order the pad shifts them out.
enum : uint8_t {
  PAD_A = 1 << 0, PAD_B = 1 << 1, PAD_SELECT = 1 << 2, PAD_START = 1 << 3,
  PAD_UP = 1 << 4, PAD_DOWN = 1 << 5, PAD_LEFT = 1 << 6, PAD_RIGHT = 1 << 7,
};

void gamepadBegin();         // starts the USB host (task on core 0)
extern volatile uint8_t gPadState;   // current PAD_* bits (SRAM, safe from any core)
static inline uint8_t gamepadState() { return gPadState; }
