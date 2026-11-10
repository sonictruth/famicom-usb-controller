// Standard-controller protocol, emulated by register polling (an interrupt's
// latency is too long for back-to-back $4016 reads, ~2 us apart):
//   OUT0 high   : pad is "strobed": D1 = A, shift position reset, buttons sampled
//   OUT0 low    : each /OE1 rising edge (end of a $4016 read) advances one bit
//   order       : A B Select Start Up Down Left Right, then 1s
// The console inverts D1, so a pressed button (bus 1) = GPIO LOW.
#include <Arduino.h>
#include "pins.h"
#include "famicom-pad.h"
#include "usb-gamepad.h"
#include "soc/timer_group_reg.h"

void padEmuBegin() {
  pinMode(PIN_OUT0, INPUT);   // no pull-ups: the shifter provides them
  pinMode(PIN_OE1, INPUT);
  pinMode(PIN_P1D1, OUTPUT);
  REG_WRITE(GPIO_OUT_W1TS_REG, M_P1D1);   // idle: reads 0
}

// The FreeRTOS tick (and any other interrupt) on this core stalls the loop
// for ~5 us, longer than OUT0's strobe pulse, so a strobe can be missed and
// the console then reads all buttons pressed (a phantom Start = pause). The
// loop therefore runs with interrupts masked. That starves the interrupt
// watchdog (TIMG1, 300 ms, normally fed by the tick), so feed it by hand
// from idle moments.
static inline void IRAM_ATTR feedIntWdt() {
  REG_WRITE(TIMG_WDTWPROTECT_REG(1), 0x50D83AA1);
  REG_WRITE(TIMG_WDTFEED_REG(1), 1);
  REG_WRITE(TIMG_WDTWPROTECT_REG(1), 0);
}

void IRAM_ATTR padEmuRun() {
  uint32_t latched = 0;
  uint32_t idx = 0;
  uint32_t prevOe = REG_READ(GPIO_IN_REG) & M_OE1;
  bool outLow = false;   // current D1 level (true = driven low = bit 1)
  const uint32_t feedEvery = ESP.getCpuFreqMHz() * 20000;   // 20 ms in cycles
  uint32_t lastFeed = ESP.getCycleCount();
  uint32_t spin = 0;
  portDISABLE_INTERRUPTS();
  for (;;) {
    if ((++spin & 0x3FF) == 0) {   // every ~1000 iterations
      uint32_t now = ESP.getCycleCount();
      if (now - lastFeed > feedEvery && !(REG_READ(GPIO_IN_REG) & M_OUT0) && (idx >= 8 || now - lastFeed > feedEvery * 4)) {
        feedIntWdt();
        lastFeed = now;
      }
    }
    uint32_t in = REG_READ(GPIO_IN_REG);
    uint32_t oe = in & M_OE1;
    if (in & M_OUT0) {
      latched = gamepadState();
      idx = 0;
    } else if (oe && !prevOe && idx < 8) {
      idx++;
    }
    prevOe = oe;
    bool bit = idx < 8 ? ((latched >> idx) & 1) : true;   // open reads give 1
    if (bit != outLow) {
      outLow = bit;
      REG_WRITE(bit ? GPIO_OUT_W1TC_REG : GPIO_OUT_W1TS_REG, M_P1D1);
    }
  }
}
