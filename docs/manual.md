# User manual for the scales (ESP32_Scale)

> How to connect, power on, weigh and service the scales built on the
> ESP32-S3. Branch: `display-oled`. Measured values — in the tables below.

## Overview

This is an electronic scale: an HX711 converter, a constant 6 kg load cell
and the ESP32-S3 user board on ESP-IDF. Interfaces — web page (HTTP),
serial console (115200 baud) and the MCP server for agents
(`tools/scale_mcp.py`). The platform is LED-lit — the center of the
platform must be free; when weighing, place the load in the center.

## Accuracy (measured)

| Parameter | Value | How obtained |
|---|---|---|
| Sampling rate | **11.3 Hz** | hardware-limited, see below |
| Single-sample noise | **110 mg** | 507 samples on a motionless platform |
| Device-side mean noise | **15 mg** | mean over ~4 s |
| Accuracy on a 73.03 g reference | **+0.14 g (+0.20 %)** | two independent pairs, 9 mg apart |
| Time to a reading | **1.15 s** | measured step response |
| Resolved value | **0.05 g** | measured against a reference of known mass |

The platform is motionless: after the weight is placed, wait until the
reading settles (the weight is measured and stable). The empty platform
can wander by **195 mg** between sessions — that is the real limiter of
accuracy, so **tare before every weighing.**

## Hardware and wiring

### HX711 → ESP32-S3

| HX711 | ESP32-S3 | Note |
|---|---|---|
| VCC | **3V3** | HX711 drives DOUT at its own supply level; on 5 V that is 5 V on GPIO16 |
| GND | GND | |
| DOUT | GPIO16 | |
| SCK | GPIO17 | |

Load cell (6 kg):

| Wire | Goes to |
|---|---|
| black | E+ |
| red | E− |
| green | A− |
| white | A+ |
| yellow | shield to GND |

Board and cell power — **3.3 V**.

### Why the rate is not higher than 11.3 Hz

The HX711 has no configuration register; the rate is set by the level on
pin 15: 0 for 10 Hz, 1 for 80 Hz. Pin 15 is tied to ground, so the
instrument runs at 11.3 Hz permanently. Rewiring would give 80 SPS but
would not improve accuracy: accuracy is set by the noise and the length of
the averaging window, not the sampling rate.

## Build and flash

```bash
source ~/esp-idf/export.sh
cd ~/ESP32_Scale
idf.py set-target esp32s3      # first build only
idf.py build

PORT=/dev/serial/by-id/usb-1a86_USB_Single_Serial_595B069397-if00
idf.py -p "$PORT" -b 460800 flash
```

**460800, not 921600** — on the CH9102 anything faster is unreliable. Before
the first flash:

```bash
cp main/credentials.h.example main/credentials.h   # fill in SSID and password
```

`main/credentials.h` is in `.gitignore` and never reaches the repository.

## WiFi

The device joins the `home4` access point and takes an address over DHCP —
currently `http://192.168.22.123/` (the serial console prints the address
it was given). Diagnostics: `s` — scan with SSID, RSSI, channel, encryption
type, bandwidth; `h` — self-test of the page.

There is no access-point fallback at the moment: the device acts as a
station only.

## Weighing

1. Let the platform settle; do not touch it.
2. Tare (button `0` or `POST /api/tare`) — every time, on an empty platform.
3. Place the load in the center and wait for the reading to settle.

## Front panel, display and buzzer (display-oled branch)

The module is a **0.96" OLED 128×64, I²C, SSD1306** (address 0x3C); the
central board has an active piezo buzzer (GPIO 47). Wiring — `docs/pinout-oled.md`:
SDA/SCL — GPIO 14/21.

### Button assignments

The ribbon — GPIO 6, 13, 8, 9, 7: contact 1 — row (GPIO 6), contacts 2–4 —
columns (GPIO 8 / 9 / 7), contact 5 — row 2 (GPIO 13).

⚠️ Ribbon contacts **4 and 5 are wired the other way round** — contact 4
is on GPIO7, contact 5 — on GPIO13. The firmware accounts for this — all
six keys (ED, SUM, ITT, SBR, 0, T) work; diagnose with the `Z` serial
command (`#BTN,<name>,row=,col=,state=` per key). The button labels in the
manual are the English equivalents of the physical Russian labels.

| Button | Short press | Hold ≥ 1.5 s |
|---|---|---|
| `ED` | cycle units: g → kg → oz → g | — |
| `SUM` | — (in the firmware "modes / calibration") | **start the calibration master** |
| `0` | tare, store the tare in NVS | — |
| `T` | tare | toggle auto-zero (AZT) |
| `ITT` | — (hold result) | — |
| `SBR` | — (reset) | — |

A long press is not less than 1500 ms (`LONG_PRESS_MS` in `main/buttons.c`);
a short one — after 20 ms of stability (4 scans × 5 ms).

### Display indicators

Chips along the bottom right (`main/oled_ui.c`; names as on the web —
STABLE, ZERO, TARE, AVG, HOLD):

| Indicator | Lights when |
|---|---|
| `STBL` | the weight is measured and stable — the same state as the **green reading flag** on the web; on the OLED — the chip + beep |
| `ZERO` | the weight is zero / the zero latch is active |
| `TARE` | the tare is active |
| `AVG` | progressive averaging is on (`avg_n ≥ 45`) |
| `HOLD` | the reading is latched |

- The weight is shown in large digits with the unit underneath: less than
  1 kg — digits + `g`; more than 1 kg — two lines (`1 kg` and the gram
  remainder with `g`).
- The piezo buzzer: a beep when the weight first settles, a warning
  sound when the weight exceeds 6 kg.

Weight check: place the reference on the platform (default 73.03 g),
hold `SUM` for 1.5 s, wait until the new zero shows. **Do not take the
reference mass from its label** — weigh it: 5 coins of 5 rubles weigh
30.63 g, not 15 g.

## Web interface

- **Zero** — the tare button.
- **Auto-zero** — toggling the auto-zero.
- The reading turns **green** when the weight is measured and stable.
- Polling — **500 ms**; readings update on their own.
- **Firmware** — update from `build/esp32_scale.bin` (progress bar).
- **Diagnostics** — `GET /api/buttons` (OLED build).
- Two-slot OTA: the update writes to the free slot, restart after
  "verified"; on failure — rollback to the previous slot.

## HTTP API

| Method and path | Purpose |
|---|---|
| `GET /` | the page |
| `GET /api/weight` | readings: `value`, `grams`, `counts`, `sigma_mg` (single-sample noise), `avg_grams`, `avg_sigma_mg`, `avg_n` — the device-side mean and its standard error |
| `POST /api/tare` | tare |
| `POST /api/unit?u=0\|1\|2` | units: 0 = g, 1 = kg, 2 = oz |
| `POST /api/azt?off` | switch auto-zero off; no parameter switches it on |
| `POST /api/cal/start[?mass=g]` | calibration wizard |
| `GET /api/cal` | calibration progress |
| `POST /api/reboot` | restart |
| `GET /api/ota` | firmware status: slot, versions, bytes written |
| `POST /api/ota?size=N` | open the transfer, erase the free slot |
| `POST /api/ota?offset=N` | send one chunk at that offset |
| `POST /api/ota?commit=1` | verify and mark the new slot to boot |
| `POST /api/ota?abort=1` | discard a partial transfer |

None require authentication — see the web interface section. `/api/weight`
— `avg_sigma_mg` shows how far the figure can be trusted.

## Serial console (115200 baud)

Poll: `python3 tools/serial_console.py --send 'R' --seconds 50`.

| Command | Action |
|---|---|
| `c` | current reading, noise, zero and stability flags |
| `t` | tare |
| `d [min]` | watch zero stability (default 20) |
| `m [g]` | paired calibration with automatic load detection |
| `w` | filter constants and state |
| `R` | dump raw samples recorded by the measurement task |
| `h` | HTTP server self-test |
| `x 0\|1` | suspend / resume the WiFi radio |
| `T <dBm>` | transmit power at runtime |
| `G 0\|1` | interrupt mask around the HX711 read |
| `s` | WiFi scan: SSID, RSSI, channel, encryption type, bandwidth |
| `k <password>` | replace the password without rebuilding |
| `b <MAC>` | bind to a specific access point, `b off` to clear |
| `p 0\|1`, `a 0..2` | PMF and authentication mode |
| `L 0\|1`, `M <MAC>` | WiFi debug log and MAC override |
| `Z` | dump of the buttons (OLED build) |
| `A <0..7>` | a beep pattern (piezo) |
| `F <grams>` | set the reference mass for one-touch calibration |

## Calibration (two points, paired method)

Empty platform — a separate point, reference — separate:

```
mass     73.066 g    reference 73.03 g
ERROR  +0.036 g  (+0.05 %)
```

- Store `cal_factor` in NVS and `tare_offset` — `m [g]` with the pair of
  points (error flag: taking the mass from the documentation instead of
  weighing it).
- `cal_factor` ≈ **425.6 counts/g** (by the 161.41 g reference — 68 692
  counts); `F` default 73.03 g.
- Check at the top: 1327 g on the kitchen scale — 426.5 against 425.2
  (discrepancy 0.3 %).

## MCP server (for agents)

`opencode.jsonc` → `mcp.servers.scale` (`python3 tools/scale_mcp.py`).
Default address `http://192.168.22.123`, override — `SCALE_URL`.

| Tool | Action |
|---|---|
| `scale_get_weight` | readings, flags, current constants, uptime |
| `scale_tare` | tare |
| `scale_set_unit` | units: 0 = g, 1 = kg, 2 = oz |

## Known issues and troubleshooting

- A multimeter cannot diagnose the cell (the signal for 73 g is about
  80 µV, and an ordinary meter reports its own error at that level).
  An oscilloscope is needed.
- The empty platform can wander by **195 mg** between sessions — tare
  before every weighing.
- The USB connection is unreliable — check that the port exists in the
  system.
- For detailed noise measurements and troubleshooting, read `AGENTS.md`
  and `docs/stage1-findings.md`.

---

The manual is supported by `README.md` (its WiFi and firmware sections
are more detailed).
