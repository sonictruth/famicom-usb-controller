// Famicom expansion-port wiring for one player (via level shifter:
// Famicom 5V = HV, ESP32-S3 3.3V = LV; plus GND, exp pin 1). All pins < 32 so
// GPIO_IN_REG / W1TS / W1TC cover them.
//
//   Exp pin | Signal       | Dir        | ESP32-S3 | Use
//   --------+--------------+------------+----------+-------------------------
//   12      | OUT0         | Fam -> ESP | GPIO4    | strobe (latch buttons)
//   14      | /OE joypad 1 | Fam -> ESP | GPIO8    | $4016 read: shift clock
//   13      | joypad 1 /D1 | ESP -> Fam | GPIO9    | $4016 bit 1: pad data
//
// The console inverts D1: line LOW reads as 1 = pressed.
#pragma once
#include <stdint.h>
#include "soc/gpio_reg.h"

constexpr int PIN_OUT0 = 4;
constexpr int PIN_OE1  = 8;
constexpr int PIN_P1D1 = 9;

constexpr uint32_t M_OUT0 = 1u << PIN_OUT0;
constexpr uint32_t M_OE1  = 1u << PIN_OE1;
constexpr uint32_t M_P1D1 = 1u << PIN_P1D1;
