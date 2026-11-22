# Pinout for the ESP32-S3 board (YD-ESP32-S3, S3-N16R8)

Unified working document for the display, buttons and HX711 wiring.
**Read the GPIO markings on the board** — they are etched on the PCB.

## 1. What connects where — short

```
ESP32-S3        Device                     Contact/marking
────────────────────────────────────────────────────────────
GPIO 2    ────  display DC
GPIO 4    ────  display RST
GPIO 5    ────  display BL  (backlight, on/off)
GPIO 6    ────  buttons, row 1   (ribbon contact 1)
GPIO 7    ────  buttons, row 2   (ribbon contact 5)
GPIO 8    ────  buttons, column 2  (contact 2)
GPIO 9    ────  buttons, column 3  (contact 3)
GPIO 10   ────  display CS
GPIO 11   ────  display SDA = SPI MOSI
GPIO 12   ────  display SCL = SPI SCLK
GPIO 13   ────  buttons, column 4  (contact 4)
GPIO 14   ────  free
GPIO 15   ────  free (was once counted as the 5th column — mistake)
GPIO 16   ────  HX711 DT   (already connected)
GPIO 17   ────  HX711 SCK  (already connected)
GPIO 47   ────  BUZZER I/O (active, 3 pins)
GPIO 48   ────  RGB LED (planned)
3V3       ────  HX711 VCC + display VCC
GND       ────  common ground of all devices
```

The buzzer connects to 3V3, GND and GPIO47 — no amplifier, no
separate power, no filters needed. It draws only a few mA and does
not pollute the power supply, unlike the amplifier.

## 2. Display GMT020-02-8P VER 1.21

2.0", ST7789V, 240×320, SPI, **3.3 V only**. Board markings (readable
either way — it's the same row):

```
BL  — CS — DC — RST — SDA — SCL — VCC — GND
GND — VCC — SCL — SDA — RST — DC — CS — BL
```

⚠️ `SDA`/`SCL` here are **SPI**, not I²C.

| Contact | Marking on ESP32 | GPIO |
|---|---|---|
| `GND` | GND | — |
| `VCC` | 3V3 | — |
| `SCL` | GPIO12 | 12 |
| `SDA` | GPIO11 | 11 |
| `RST` | GPIO4 | 4 |
| `DC` | GPIO2 | 2 |
| `CS` | GPIO10 | 10 |
| `BL` | GPIO5 | 5 |

The display is rotated 90° in the panel window, so LVGL works with
**320×240**. The orientation/inversion is set by the macros
`ST7789_SWAP_XY`, `ST7789_MIRROR_X`, `ST7789_MIRROR_Y`, `ST7789_INVERT`
in `main/st7789.h`.

The `VCC` input of the display — 100 µF + 100 nF right at the connector.

## 3. Buttons — matrix 2×3, 6 buttons

From the panel ribbon. Refined continuity check (2026-11-21): pairs
`1-2`, `1-3`, `1-4` and `2-5`, `3-5`, `4-5`. So rows — contacts `1` and
`5`, columns — `2`, `3`, `4`.

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

Scanning rule: the active row = **0**, the inactive row — **high-Z (input)**.
The inactive row cannot be driven high: with two buttons pressed in one
column, you would get a short. The columns are read by an active low.

Actions:

| Button | Short | Long (>1.5 s) |
|---|---|---|
| `ED` | cycle g / kg / oz | — |
| `0` | tare | — |
| `T` | tare | toggle AZT |
| `SUM` | back to the main screen | calibration (reference from NVS) |
| `ITT` | back to the main screen | — |
| `SBR` | back to the main screen | — |

Set the reference mass with the `F <grams>` command (default 73.03 g).

## 4. Buzzer on GPIO47 (active, 3 pins)

An ML module with the marking `VCC / I/O / GND` and the label «低电平触发»
(activated at a low level). It has a generator inside, so **the frequency
cannot be changed** — the firmware only turns it on/off, making beeps
and pauses.

| Buzzer pin | Where |
|---|---|
| `VCC` | **3V3** |
| `GND` | GND |
| `I/O` | **GPIO47** |

Check: `A <0..7>` — plays one of 8 sound signals (0 tare, 1 stable,
2/3 AZT, 4 calibration start, 5 put the calibration weight, 6
calibration ready, 7 error).

## 5. Reserved pins of this board

| GPIO | Reason |
|---|---|
| 0, 3, 45, 46 | strapping |
| 26–37 | Octal PSRAM / flash |
| 43, 44 | UART0 (console and flashing) |
| 19, 20 | native USB (reserve) |
| 14 | pulled out, unused in the project (4th button column → GPIO13) |

## 6. Core distribution

| Core | Busy |
|---|---|
| Core 0 | `weight_task` (HX711), console — nothing else |
| Core 1 | WiFi, lwIP, HTTP, LVGL, UI task, buttons, buzzer, calibration master |

All new tasks are pinned to Core 1; `portENTER_CRITICAL` around the
HX711 read masks interrupts only on Core 0, so SPI does not interfere
there.

## 7. Console commands for checking

| Command | Action |
|---|---|
| `Z` | read state of all 6 buttons |
| `Y 0\|1` | turn the backlight off / on |
| `A <0..7>` | play sound: 0 tare, 1 stable, 2 AZT on, 3 AZT off, 4 calibration start, 5 put calibration weight, 6 calibration ready, 7 error |
| `F <g>` | set the reference mass |
| `c`, `R`, `d` | as before: reading, raw samples, zero control |
