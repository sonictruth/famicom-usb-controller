# Famicom USB controller

ESP32-S3 firmware: a USB HID gamepad on the native USB (OTG) port acts as joypad 1 on the Famicom expansion port. 

## Pinout

Famicom expansion port to ESP32-S3, through a level shifter (Famicom 5V = HV, ESP32-S3 3.3V = LV).

```
                                ┌──────────────────────────────┐
                       3.3 V ───┤ LV                         HV ├── 5V  (exp pin 2)
                        GND ────┤ GND                        GND ├── GND (exp pin 1)
                                │                              │
  ESP32-S3                      │     8-channel level shifter  │                  Famicom expansion
  GPIO4 ◄───────────────────────┤ B0 ◄► A0                     ├─◄ pin 12   OUT0     strobe (latch)
  GPIO8 ◄───────────────────────┤ B1 ◄► A1                     ├─◄ pin 14   /OE j1   $4016 read, shift clock
  GPIO9 ────────────────────────┤ B2 ◄► A2                     ├─► pin 13   j1 /D1   $4016 bit 1, pad data
                                └──────────────────────────────┘
```

Arrowheads show signal direction as used (Fam -> ESP for OUT0 and /OE1, ESP -> Fam for /D1); 3 of the 8 shifter channels are used.

| Exp pin | Signal | Dir | ESP32-S3 | Use |
|---|---|---|---|---|
| 12 | OUT0 | Fam -> ESP | GPIO4 | strobe (latch buttons) |
| 14 | /OE joypad 1 | Fam -> ESP | GPIO8 | `$4016` read, shift clock (active low) |
| 13 | joypad 1 /D1 | ESP -> Fam | GPIO9 | `$4016` bit 1, pad data (low = pressed) |


## USB OTG power

The gamepad plugs into the ESP32-S3's native **USB** port, keep the **UART/COM** port for flashing and the serial log. Host mode needs 5V on VBUS, which ESP32-S3-DevKitC-1 boards (and their two-USB-C clones) don't supply by default. Bridge both solder jumpers on the back of the board with solder:

- **USB-OTG**
- **IN-OUT**

## Protocol

Protocol: OUT0 high latches the buttons; each `$4016` read (/OE1 rising edge) shifts A, B, Select, Start, Up, Down, Left, Right, then 1s. The loop polls GPIO registers on core 1; USB runs on core 0.

## Build

```sh
pio run                 # build
pio run -t upload         # flash (UART/COM port)
pio device monitor        # serial console, 115200
```

Button map (`src/usb-gamepad.cpp`): HID buttons 1/3/5 = B, 2/4/6 = A, 7/9 = Select, 8/10 = Start; hat, D-pad usages and left stick X/Y = D-pad.

Set `build_flags = -DHID_RAW_DEBUG` in `platformio.ini` to also dump raw HID reports (`[hid] ...`) when the first 8 bytes change, for adding support for a new pad. Off by default to keep the serial port quiet.

## Timing notes

The pad emulation is a polling loop on core 1 with interrupts masked: any interrupt there (the FreeRTOS tick) pauses it for ~5 us, longer than OUT0's strobe pulse, and a missed strobe makes the console read every button as pressed (a phantom Start pauses games). Serial and the USB host run from a core-0 task. With the tick masked, the interrupt watchdog is fed by hand from idle moments (`feedIntWdt()` in `src/famicom-pad.cpp`); do not add code on core 1 that needs interrupts.

`-DPAD_FILTER_DOWN_DIAGONALS` (on by default, see `platformio.ini`): NES Tetris and similar games cancel soft drop on a Down+Left/Right frame and then ignore Down until it is pressed afresh, so rolling a thumb Down-Right-Down did nothing. With the flag a Down diagonal is sent as its sideways half only, and when it starts from plain Down it is held back 30 ms (`DIAG_DELAY_MS`), so a quick roll sends nothing extra. Remove the flag for a plain pass-through. Several `-D` flags go in one `build_flags` line; `-DHID_RAW_DEBUG` dumps raw HID reports for supporting a new pad.
