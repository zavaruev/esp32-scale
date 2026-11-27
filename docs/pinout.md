# ESP32-S3 pinout (YD-ESP32-S3, S3-N16R8)

Single working reference for the display, buttons and HX711 wiring.
**Follow the GPIO markings on the board** - they are etched on the PCB.

## 1. Quick wiring table

```
ESP32-S3        Device                  Contact / marking
---------------------------------------------------------
GPIO 2    ----  display DC
GPIO 4    ----  display RST
GPIO 5    ----  display BL  (backlight, on/off)
GPIO 6    ----  buttons, row 1   (ribbon contact 1)
GPIO 7    ----  buttons, row 2   (ribbon contact 5)
GPIO 8    ----  buttons, column 2  (contact 2)
GPIO 9    ----  buttons, column 3  (contact 3)
GPIO 10   ----  display CS
GPIO 11   ----  display SDA = SPI MOSI
GPIO 12   ----  display SCL = SPI SCLK
GPIO 13   ----  buttons, column 4  (contact 4)
GPIO 14   ----  free
GPIO 15   ----  free (once mistaken for a 5th column)
GPIO 16   ----  HX711 DT   (already connected)
GPIO 17   ----  HX711 SCK  (already connected)
GPIO 47   ----  BUZZER I/O (active, 3 pins)
GPIO 48   ----  RGB LED (planned)
3V3       ----  HX711 VCC + display VCC
GND       ----  common ground for all devices
```

The buzzer connects to 3V3, GND and GPIO47. No amplifier, separate supply or
filters are needed; it draws a few mA and does not couple noise into the
power rail as an amplifier would.

## 2. Display GMT020-02-8P VER 1.21

2.0" ST7789V, 240x320, SPI, **3.3 V only**. Silkscreen on the module (either
reading direction, it is the same row):

```
BL  - CS - DC - RST - SDA - SCL - VCC - GND
GND - VCC - SCL - SDA - RST - DC - CS - BL
```

Warning: `SDA`/`SCL` here are **SPI** lines, not I2C.

| Contact | On ESP32 | GPIO |
|---|---|---|
| `GND` | GND | - |
| `VCC` | 3V3 | - |
| `SCL` | GPIO12 | 12 |
| `SDA` | GPIO11 | 11 |
| `RST` | GPIO4 | 4 |
| `DC` | GPIO2 | 2 |
| `CS` | GPIO10 | 10 |
| `BL` | GPIO5 | 5 |

The panel is mounted rotated 90 degrees in the front-panel window, so LVGL
works in **320x240**. Direction/inversion come from `ST7789_SWAP_XY`,
`ST7789_MIRROR_X`, `ST7789_MIRROR_Y` and `ST7789_INVERT` in `main/st7789.h`.
A 100 uF + 100 nF pair sits at the display VCC connector.

The panel applies BGR channel order and inversion. `main/ui.c` uses
pre-distorted framebuffer colours so the displayed colours are correct;
see the comment above the `COLOR_*` defines before changing them.

## 3. Buttons - 2x3 matrix, 6 keys

Ribbon from the front panel. Verified continuity (2026-11-21): pairs `1-2`,
`1-3`, `1-4` and `2-5`, `3-5`, `4-5`. Rows are contacts `1` and `5`; columns
are `2`, `3`, `4`.

| Ribbon contact | Role | GPIO | Mode |
|---|---|---|---|
| 1 | row 1 | 6 | output |
| 5 | row 2 | 7 | output |
| 2 | column 2 | 8 | input + pull-up |
| 3 | column 3 | 9 | input + pull-up |
| 4 | column 4 | 13 | input + pull-up |

```
              c2(8)   c3(9)   c4(13)
   r1(6)       ED      SUM      0
   r5(7)       T       ITT      SBR
```

Scanning rule: active row is driven **LOW**, inactive rows stay high-Z
(input). Driving an inactive row HIGH would short two rows when two keys in
one column are pressed together. Columns are read active LOW.

Key actions (short press / long press >1.5 s):

| Key | Short | Long |
|---|---|---|
| `ED` | toggle AUTO (g below 1 kg, kg above) / ounces | - |
| `0` | zero the reading | - |
| `T` | tare | AZT on/off |
| `SUM` | start calibration | start calibration |
| `ITT` | hold / release the result | - |
| `SBR` | release hold, back to weighing | back to weighing |

Reference mass is set with `F <grams>` (73.03 g by default).

## 4. Buzzer on GPIO47 (active, 3 pins)

An ML module marked `VCC / I/O / GND` and labelled active-low. It has an
internal oscillator, so **the frequency cannot be changed** - firmware only
turns it on and off to form beeps and pauses.

| Buzzer pin | Where |
|---|---|
| `VCC` | **3V3** |
| `GND` | GND |
| `I/O` | **GPIO47** |

Test with `A <0..7>`: 0 tare, 1 stable, 2/3 AZT, 4 calibration start,
5 place weight, 6 calibration done, 7 error.

## 5. Forbidden pins on this board

| GPIO | Reason |
|---|---|
| 0, 3, 45, 46 | strapping |
| 26-37 | Octal PSRAM / flash |
| 43, 44 | UART0 (console and flashing) |
| 19, 20 | native USB (reserved) |
| 14 | exposed but unused (button column 4 moved to GPIO13) |

## 6. Core placement

| Core | Owned by |
|---|---|
| Core 0 | `weight_task` (HX711) and the console - nothing else |
| Core 1 | WiFi, lwIP, HTTP, LVGL, UI task, buttons, buzzer, calibration wizard |

All new tasks are pinned to Core 1. `portENTER_CRITICAL` around the HX711 read
masks interrupts on Core 0 only, so SPI traffic on Core 1 cannot disturb it.

## 7. Console commands for verification

| Command | Action |
|---|---|
| `Z` | read all six buttons |
| `Y 0\|1` | backlight off / on |
| `A <0..7>` | play a sound: 0 tare, 1 stable, 2 AZT on, 3 AZT off, 4 calibration start, 5 place weight, 6 calibration done, 7 error |
| `F <g>` | set the reference mass |
| `c`, `R`, `d` | reading, raw counts, zero stability - as before |
