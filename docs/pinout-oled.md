# Pinout for the OLED variant (display-oled branch)

OLED 0.96" 128×64, I²C, SSD1306, 4 wires.

## OLED connections

| OLED | ESP32-S3 |
|---|---|
| VCC | 3V3 (most modules have a regulator and tolerate 5 V, but 3.3 V is safer) |
| GND | GND |
| SDA | **GPIO 14** |
| SCL | **GPIO 21** |

I²C address: **0x3C** (if it doesn't work — try 0x3D).

## The rest (unchanged)

| Device | GPIO |
|---|---|
| HX711 DT / SCK | 16 / 17 |
| Buzzer (active) VCC / GND / I/O | 3V3 / GND / 47 |
| Buttons: row 1 | GPIO 6 (ribbon contact 1) |
| Buttons: row 2 | GPIO 13 (ribbon contact 5) |
| Buttons: columns | GPIO 8 / 9 / 7 (contacts 2 / 3 / 4) |

⚠️ Ribbon contacts **4 and 5 are wired the other way round** — contact 4
goes to GPIO7, contact 5 — to GPIO13. The firmware accounts for this
(commit 131f482). All 6 keys confirmed working: ED, SUM, ITT, SBR, 0, T.
Button `0` takes the tare and stores it in NVS.

## ⚠️ GPIO14 conflict

In the OLED variant **GPIO14 = SDA**, so it is not used for buttons.
If you go back to GMT020 (branch `display-gmt020`), GPIO14 is free there.

## Interface

The weight is shown in large digits (5×7 font, scale 2×), with the unit
underneath; above 1 kg the display shows two lines (`1 kg` and the gram
remainder). The hold logic is the same as on the web.

## Two branches

- `display-gmt020` — GMT020-02-8P (ST7789 240×320) + LVGL. Kept for a new
  module — the first module failed after a few minutes of operation.
  Commit d18bd18.
- `display-oled` — current: OLED 128×64 I²C, its own renderer without LVGL.

Switch: `git checkout display-gmt020` / `git checkout display-oled`.
