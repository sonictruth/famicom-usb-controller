// Famicom USB gamepad adapter (ESP32-S3): a USB HID gamepad on the OTG port
// appears to the Famicom as joypad 1 on the expansion port. Pressed buttons
// are also logged on the serial console (115200). Wiring: include/pins.h.
// Build with `pio run`.
//
// Core 1 runs only the pad emulation loop: Serial and the USB host are
// started from a core-0 task, because esp_intr_alloc() binds an interrupt
// (UART0, USB OTG) to the core that calls it, and any interrupt on core 1
// stalls the loop for microseconds -- long enough to miss OUT0's strobe pulse.
#include <Arduino.h>
#include "pins.h"
#include "usb-gamepad.h"
#include "famicom-pad.h"

static SemaphoreHandle_t ready;

static void startup(void *) {
  Serial.begin(115200);
  Serial.println("famicom usb pad, build " __DATE__ " " __TIME__);
  gamepadBegin();
  vTaskDelay(pdMS_TO_TICKS(200));
  Serial.flush();
  xSemaphoreGive(ready);
  vTaskDelete(nullptr);
}

void setup() {
  ready = xSemaphoreCreateBinary();
  xTaskCreatePinnedToCore(startup, "startup", 8192, nullptr, 2, nullptr, 0);
  xSemaphoreTake(ready, portMAX_DELAY);
  padEmuBegin();
  padEmuRun();   // never returns
}

void loop() {}
