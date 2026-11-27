# AGENTS.md — ESP32_Scale

High-precision electronic scale on ESP32-S3. Custom firmware, ESP-IDF, NOT ESPHome.
Status: **stage 1 closed**, the hardware is judged usable.

## Quick start

```bash
source ~/esp-idf/export.sh        # idf.py is not in PATH without this
cd ~/ESP32_Scale
idf.py build
PORT=/dev/serial/by-id/usb-1a86_USB_Single_Serial_595B069397-if00
idf.py -p "$PORT" -b 460800 flash  # 921600 is unreliable on the CH9102

stty -F "$PORT" 115200 raw -echo && timeout 40 cat "$PORT" > logs/run.log
python tools/analyze_stage1.py logs/run.log --cal-factor 425.2
```

Details of the measurements and a write-up of the gotchas — `docs/stage1-findings.md`.
The chip's datasheet is right next to it: `docs/hx711_english.pdf`.

## Current release and OTA safety

`main` tracks the firmware that is actually on the device:
**v1.4.3-auto-units**, built from the ST7789 source. `version.txt` holds the
string reported by `GET /api/ota`. The older OLED source lives on the
`display-oled` branch; the two display stacks are not interchangeable.

**Before every OTA, read `run_ver`:**
```bash
curl -s http://192.168.22.123/api/ota
```
An OLED image flashed onto ST7789 hardware makes `ssd1306_init()` fail, the
device drops off the network and the screen goes dark; `ota_init()` had already
accepted the image, so rebooting will not revert it. Build from the worktree
whose version matches `run_ver`, then push with `tools/ota_push.py`.

The host has no `idf.py`/`python3`/`git`. Build inside a container from
`esp-idf-6.0.1-local:latest` with the source mounted at `/project` and use the
`idf` wrapper: `node /mnt/media/docker-compose/opencode/.home/dexec.js <container> idf build`.

## HX711 pinout

DT → GPIO16, SCK → GPIO17, VCC → **3V3**, GND → GND.
Load cell: E+ black, E- red, A- green, A+ white, shield yellow to GND.
Load strictly in the centre of the platform.

**Why 3.3 V and not 5 V:** HX711 drives DOUT at the level of its own supply. With
5 V, 5 V arrives at GPIO16 of the ESP32-S3, which is unacceptable to it.

## Calibration (2 references in one session, 2026-10-07)

| Reference | Count delta | `cal_factor` |
|---|---|---|
| Jar **161.41 g** (pharmacy scale) | 68 692 ±4.7 | **425.57** ±0.03 |
| Coins **30.63 g** (pharmacy scale) | 13 014 ±70.6 | **424.86** ±2.31 |

**`cal_factor` = 425.2 counts/g**, accuracy ±0.01% on the jar. The discrepancy
between the references is 0.6% — the instrument and its mounting are fine. Logs: `logs/v2_*.log`.

| Point | Median | σ |
|---|---|---|
| Tare-1 | 418 815 | 55.1 |
| Jar | 487 502 | 45.3 |
| Coins | 431 775 | 44.5 |
| Tare-2 | 418 749 | 85.9 |

- **`tare_offset` = 418 815 counts** (plate in place, platform empty)
- **Tare walk-off −66 counts = −0.16 g → no hysteresis**
- 1 count = **2.35 mg**
- 6 kg = 2.55 million of 8.39 million = **30% of the HX711 range**, plenty of headroom

### ⚠️ The earlier 871 counts/g were wrong by a factor of two

The first measurement divided the delta by **15 g**, taken from memory as the
weight of 5 coins of 5 rubles each. In reality 5 coins weigh **30.63 g**. The error was
exactly twofold, which made every accuracy calculation understate the resolution by a factor of two.

**Never take the mass of a reference from memory or from its denomination — only weigh it.**
The second reference in the same session caught the error; with a single reference it would have gone into the firmware.

### Requirement for a working reference

Pharmacy scale: limit **200 g**, resolution 0.01 g. Kitchen scale: 1327 g, specified as 1 g,
**in fact ±1 g** (two measurements of the same mass differed by 0.72 g).

Neither instrument covers both tasks: the pharmacy scale is accurate (0.01%) but
small (3.3% of range); the kitchen scale is large (22%) but coarse (0.054%). Working scheme:

- **`cal_factor` = 425.2 is set by the pharmacy scale** using the 161 g reference
- **1327 g on the kitchen scale checks the top of the range**, without setting the coefficient
- check passed: 426.5 against 425.2, discrepancy 0.3%

Honest accuracy class with the current references: **~0.05 g at the bottom of the
range, ~0.2 g at 1.3 kg.** Weights of class 0.1% will close the gap.

### Accuracy (recalculated for 425.2)

σ of the raw count = **45 counts = 106 mg**, at 11.3 Hz.

| α | Noise after the filter | Settling | Resolution |
|---|---|---|---|
| 0.05 | 12.7 mg | 5.3 s | 0.05 g |
| 0.10 | 21.9 mg | 2.7 s | 0.05 g |
| 0.15 | 29.0 mg | 1.8 s | 0.1 g |
| 0.20 | 34.8 mg | 1.3 s | 0.1 g |
| 0.30 | 44.4 mg | 0.9 s | 0.1 g |

**0.01 g at 11.3 Hz is unreachable:** it needs N > 448 counts, that is **40
seconds** of averaging. The earlier estimate that "0.01 g is achievable" was a
consequence of an inflated `cal_factor`. In reality — **0.05 g at rest, 0.1 g in motion.**

**This changes the specification:** the specified 0.01 g is unreachable at 11.3 Hz
neither by filtering nor by improving the hardware. Either we target 0.05 g, or we return to the question of 80 SPS.

## The rate is set by the hardware, not by the code

**There is no configuration register on this chip.** Datasheet Table 1, pin 15:

```
15  RATE   Digital Input   0: 10 Hz; 1: 80 Hz
```

The rate is determined by the **level on physical pin 15** and cannot be
changed by software. That is exactly why sweeping all 128 bytes gave nothing (maximum 15.6 Hz).

Channel and gain are not register-based either — they are set by the **number of
PD_SCK pulses** per period (Table 3): 25 → channel A / gain 128, 26 → B / 32,
27 → A / 64. Outside 25–27 — communication error. The driver issues exactly 25 and writes no configuration.

## Decided: no soldering, 11.3 Hz permanently

Pin 15 stays shorted to ground. Settling time 0.5 s → ~1 s.

The refusal to re-solder is justified **purely by response time**: 80 SPS
would give speed, but not accuracy. The real accuracy at 11.3 Hz is **0.05 g at
rest** (see the table above), and it does not depend on the sampling rate but is
determined by σ and the length of the filter window.

⚠️ But after the `cal_factor` correction it became clear: **0.01 g is unreachable
with any filter setting at 11.3 Hz** — it needs 40 seconds of averaging. No
decision has been taken on this point, see "Not established".

## The filter was verified offline — and exposed the main limitation

`tools/filter_sim.py` — an executable specification of the chain median(5) →
adaptive EMA → AZT → deadband. Run on real logs.

| Load | Reading | Error |
|---|---|---|
| coins 30.63 g | 30.487 g | −0.143 g |
| jar 161.41 g | 161.532 g | +0.122 g |

Noise after the filter is 40–65 mg, but the errors are **±0.12–0.14 g and of
different sign**. A coefficient error would shift both the same way. Different signs mean **zero float** is to blame.

### Zero float measured (over 35 s)

| Log | Counts | Grams |
|---|---|---|
| tare1 | −27 | −0.063 g |
| jar | +19 | +0.045 g |
| coins | +40 | +0.094 g |
| 1327 g | −58 | −0.136 g |
| **tare2** | **+214** | **+0.503 g** |

### AZT works only inside the window

| Log | Result |
|---|---|
| tare2 (monotonic drift +6.1 count/s) | holds **0.000 g**, spread 26.5 mg, ZERO 98% |
| tare1 (drift −0.78 count/s) | drifts to −0.245 g, ZERO 3% |

### The conflict that blocks 0.05 g

The AZT window must be **≥0.5 g**, otherwise the zero cannot be held. A 0.05 g
load is 21 counts, **10 times smaller than the window**. Any window that holds the zero will swallow a small load.

**AZT and 0.05 g resolution are mutually exclusive on this hardware.** The
bottleneck is not the filter, not `cal_factor` and not the rate — it is zero
float of up to ±0.5 g, 10 times larger than the target resolution.

The honest specification right now: **~0.15 g at rest, ~0.2 g under load.**

To get to 0.05 g, the drift has to be reduced, not the filter improved:
- clean 3.3 V supply, the cheap regulator on the module is suspect
- capacitors on the supply and the module's reference output
- let the load cell reach thermal equilibrium before taring (`tare2` +214 counts
  looks like warm-up)
- mechanically secure the platform so that it does not creep

## Network and web interface (2026-10-07)

### Architecture

| Module | Role |
|---|---|
| `main/hx711.{c,h}` | HX711 driver, 25 clocks, no configuration write |
| `main/scale_filter.c` | median(5) → adaptive EMA → AZT → deadband |
| `main/weight_task.c` | measurement task on **Core 0**, NVS, snapshot under a mutex |
| `main/web_server.c` | WiFi STA + HTTP + page, runs on Core 1 |
| `main/scale_app.c` | initialisation and serial console |

**Ownership rule:** `weight_task` is the only reader of the HX711. All serial
commands go through the snapshot, nobody touches the bus directly. Two tasks
clocking one HX711 would interleave the pulse sequences.

### WiFi password

`main/credentials.h` is **in `.gitignore`**. The template
`main/credentials.h.example` is in the repository. Currently: SSID `home4`, the
password lives only in `main/credentials.h` (in `.gitignore`). **It must not be
written here** — that file goes into the repository. Substitute it on the fly: `k <new password>`.

### STA to home4 — CONNECTED (2026-10-07)

The cause of the long failure was **not the password**: the WiFi calibration in
NVS was corrupted, `phy_init` logged `Saving new calibration data due to checksum
failure`. The handshake did not get through with any variant of the password.
After `esptool erase_flash` the network came up on the first attempt.

```bash
python -m esptool --port "$PORT" --baud 460800 erase_flash
idf.py -p "$PORT" -b 460800 flash
```

Success: `wifi:security: WPA2-PSK-SHA256, rssi: -69, pmf:1`, address `192.168.22.123`.

**If WiFi stopped connecting after a series of flashes — do `erase_flash`
first, rather than changing the password and the parameters.**

### Order of the WiFi calls — a trap with a silent failure

```c
wifi_config_want_connect = false;
esp_wifi_set_mode(WIFI_MODE_STA);   /* starts the connection by itself! */
esp_wifi_set_config(WIFI_IF_STA, &wc);
esp_wifi_start();
esp_wifi_connect();
```

`esp_wifi_set_mode()` **initiates the connection by itself**. The subsequent
`set_config` is rejected with `wifi:sta is connecting, cannot set config` /
`ESP_ERR_WIFI_STATE`, and **the password never reaches the chip** — the attempt
proceeds without authentication, which looks like reason=211 "AP not found".
Auto-connection must be suppressed with a flag that the `WIFI_EVENT_STA_START` handler reads.

The same order is mandatory in `wifi_set_auth_mode()`: `stop` → `set_mode` →
`set_config` → `start`. Without `set_mode` after `stop` we get `ESP_ERR_WIFI_MODE`.

### Other parameters set explicitly

- `country = RU, ch 1..13, policy MANUAL` — by default IDF sets the world-safe
  "01" with `nchan=11`, and home4 is exactly on channel 11, that is right at the edge
- `scan_method = WIFI_ALL_CHANNEL_SCAN`, `sort_method = WIFI_CONNECT_AP_BY_SIGNAL`,
  `threshold.rssi = -80`
- PMF configurable at runtime; with it enabled reason=2, disabled reason=7.
  By default we leave it enabled — it worked
- Diagnostics of causes: the reason from the disconnect event, not the internal
  words `0xb0`/`0x200` from the state machine log

### Diagnostic commands

| Command | Action |
|---|---|
| `s` | WiFi scan: SSID, RSSI, channel, encryption type, bandwidth |
| `h` | HTTP server self-test |
| `k <password>` | substitute the password without rebuilding |
| `p 0..1` | PMF on/off with reconnection |
| `a 0..2` | WPA2 / WPA3-SAE / WPA2-WPA3 |

### Earlier (no longer relevant)

Causes observed before the flash erase:

| reason | Meaning |
|---|---|
| 2 | AUTH_EXPIRE — the AP drops the station in the 4-way handshake |
| 201 | NO_AP_FOUND |
| 211 | NO_AP_FOUND_IN_AUTHMODE_THRESHOLD |

Checked and did not help: PMF capable, the WPA2-WPA3 threshold, WPA3-SAE.
**The password has to be verified with another device** (a phone) — the ESP32-S3
does not connect to a WPA2 network that is visible in the air.

### Fallback access point mode

If the STA has not connected within 15 s, the scale brings up an **open** access
point `ESP32-Scale` and serves the interface at `http://192.168.4.1`. This makes
it possible to test the UI on the bench independently of the home4 problem.

### Interface check

The `h` command makes an HTTP request to itself and prints the status and the size:

```
#SELFTEST,status=200,declared_bytes=5938
#SELFTEST,OK page served
```

**Always check after editing HTML:** a lost quote in a CSS line compiles
silently and serves a broken page. This has happened before.

### Serial commands

| Command | Action |
|---|---|
| `c` | current reading, σ, state |
| `t` | tare |
| `d [min]` | zero stability monitor |
| `m [g]` | paired calibration with auto-detect of the load |
| `w` | filter constants and state |
| `R` | dump of the raw counts recorded by the measurement task itself |
| `x 0\|1` | switch the WiFi radio off/on (for measuring noise) |
| `s` | WiFi scan with encryption type and bandwidth |
| `h` | HTTP server self-test |
| `a 0..2` | authorisation mode: WPA2 / WPA3-SAE / WPA2-WPA3 |
| `p 0\|1` | PMF on/off |
| `k <password>` | substitute the password without rebuilding |
| `b <MAC>` | bind to a specific access point, `b off` — clear it |
| `L 0\|1` | debug log of the WiFi driver |
| `G 0\|1` | interrupt mask around the HX711 read (for A/B) |
| `M <MAC>` | substitute the station MAC |


## WiFi interference spoiled all measurements (solved 2026-10-08)

### What was observed

Readings on an empty platform wandered by ±0.6 g, and sometimes `-1` and
`0x3FFFFF` appeared among the counts. I first decided the 40-count deadband was
to blame and proposed widening it to 200. **That would have been a mistake:** I was measuring noise from contaminated data.

### The real cause

`esp_rom_delay_us()` is a **calibrated cycle on the CPU, not a hardware timer**.
Any interrupt that lands inside simply stretches the delay. That means the SCK
period became indeterminate, the chip lost count of its own clocks, and the read
returned `-1` or `0x3FFFFF` — exactly the symptom that gotcha №1 blames on a
forgotten 25th pulse. The driver was written correctly.

WiFi runs on the same Core 0 as the measurement task, so every burst of
transmission landed right inside the read window.

### The measurement that proved it

Empty platform, fraction of corrupted samples (5σ threshold):

| Condition | Before the fix | After the fix |
|---|---|---|
| Radio off | 0 of 507 | — |
| Radio, no HTTP | 3 of 463 (0.65 %) | — |
| Radio, HTTP every 100 ms | **19 of 185 (10.3 %)** | **4 of 438 (0.9 %)** |

Raw spread on an empty platform: **3 775 516 counts → 582 counts.**

### The fix

1. The whole 24-bit read and the 25th pulse are wrapped in `portENTER_CRITICAL`.
2. `HX711_SETTLE_US` reduced 20 → 5 µs. The datasheet allows an SCK period down
   to 1 MHz (1 µs), so 5 µs is a fivefold margin. The window with interrupts
   disabled became ~250 µs instead of ~1 ms; the mask is held only for the time
   of the read, the wait for DOUT remained blocking.
3. Web page polling 400 ms → 1500 ms.
4. Read failures with DOUT high for more than 250 ms got a 50 ms backoff. Without
   it a `continue` with no delay turned a stuck DOUT into 100% load on Core 0.

### Moving WiFi to Core 1 — the story of the decision

First I pinned the WiFi task to Core 1, reasoning that since the interference was
proven, WiFi interrupts should not be let into the same core where the read runs.
That did not help at the time, and I rolled it back to Core 0, deciding the move
hurt connection speed. Later it turned out that the rollback was premature: the
cause lay not in the cores but in a non-working transmit power limit.

Later the move did come back and stayed. The argument now is not "remove the
interference" but a different one, and it is testable: `portENTER_CRITICAL` on a
dual-core Xtensa masks interrupts **only on the core where it runs**. The
measurement task lives on Core 0, so with WiFi on Core 1 its interrupts are not
touched by the mask, and the read protection is preserved in full.

Both settings (Core 1 and 8 dBm) were verified together: 200 seconds without a
single drop. I did not run an isolated A/B on the core alone, so the contribution
of each factor separately has not been measured — only the whole combination.

### About the WDT panic — it was an artefact of the measurement

While readings were being polled at a 5 ms rate, `task_wdt` panicked every 5 s:
`CPU 0: main`, `IDLE0` did not reset the watchdog. Over 75 s of normal operation —
**zero trips**. Reason: `taskYIELD()` does not hand over to the idle tasks,
because their priority is 0, and yield only switches to an equal or higher
priority. The wait for DOUT was moved to `vTaskDelay(1)` after a short spin.

**A conclusion worth remembering: the measuring instrument was changing what it
measured.** Polling the snapshot with priority 6 kept both of the core's queues
non-empty. So now the raw counts are written by the measurement task itself into
a ring buffer, and the console only dumps them with `R` — no polling at all.


## There is no drift — the earlier entry was wrong (corrected 2026-10-09)

### What had been written

Previously this said: "the zero wanders by ±0.4 g in steady state and by ±1.8 g
in the first 30 seconds after a touch", and on that basis the conclusion was
built that drift is the main limiter of accuracy.

**That was wrong.** The figures came from measurements made in unsuitable
conditions, and I took three different things for drift.

### What a normal measurement showed

Ten minutes, fresh tare, platform untouched, polling over HTTP every 3 s:

| | |
|---|---|
| median | 419 013 |
| σ | 43.9 counts = 103 mg |
| spread | 258 counts = 0.6 g |
| median by quarters | 419 000 → 419 039 → 419 016 → 418 994 |
| walk-off from start to end | **−3 counts** |
| trend | **+3 mg/min** |

Zero trend and a constant median. **The resting point is stable within ±0.11 g
over ten minutes.** Neither warm-up nor creep.

### Where the "±0.4 g" came from

1. **Wander, not drift.** The wander σ is 103 mg, and the spread of 258 counts
   is the tail of random stepping, not a systematic offset. The noise is correlated
   in time, and I took it for a slow walk-off.
2. **Touches to the platform.** In one of the measurements there was an outlier of
   +3 300 counts = +7.8 g on an empty platform. It went into the "drift" in full.
3. **A tare taken during settling.** The tare on the 25th second after boot gave
   418 943, a manual one right after — 419 654, the platform settled to 418 900.
   The tare was taken 1.8 g above rest. That is not drift, it is bad methodology.

Separately verified that WiFi creates no drift: 4 minutes with the radio on —
spread of 159 counts, with it off — 125.

### Why it all looked like a breakdown

The reading on an empty platform **stuck at −0.10 g** and would not show zero.
The cause was neither in the hardware nor in the noise:

```c
#define AZT_WINDOW 40.0
#define DEADBAND   AZT_WINDOW      /* the same constant */
...
if (fabs(ema - anchor) > AZT_WINDOW) {
    anchor = ema - (ema > anchor ? AZT_WINDOW : -AZT_WINDOW);
}
```

The auto-zero pinned the anchor exactly to `AZT_WINDOW`, which is the same
magnitude as the deadband. The reading ended up exactly where the decision "is
this zero or not" is made: 40.000000 counts reads as zero, 40.000001 — as 0.09 g.

### The fix

Inside the deadband the anchor is set equal to the reading, instead of being
pulled towards it at a rate of 6 counts/s:

```c
if (fabs(ema - anchor) <= DEADBAND) {
    anchor = ema;          /* exactly zero */
}
```

The sticking disappears completely: the zero comes out exact, and a real load
drives the reading well beyond the deadband, and tracking resumes as before.

In the web interface the freeze of the reading yields to zero: if the device
reports `zero`, the frozen digit is reset, otherwise the screen would keep showing
a value the device no longer believes.

### Result

- Empty platform, 35 measurements over 200 seconds: **35 out of 35 exactly 0.000 g**.
- Weight 73.04 g: mass 73.106 g, error **+0.066 g (+0.09 %)**.

### What remained the real limiter

Not drift, but **noise σ = 103 mg**. It is what determines that the device cannot
show 0.01 g: with such a σ a tolerance of ±5 mg would require 40 seconds of
averaging. Further work has to be on the noise and on touches to the platform,
not on the drift, which does not exist.


## WiFi: "auth expired" caused by a non-working power limit (solved 2026-10-08)

### Symptom

The station saw `home4` (18 networks in the air, `home4` at −69…−74 dBm, WPA2-PSK,
channel 11), the password was correct — a phone connected. But every authentication
attempt broke off exactly after **1010 ms**, code `reason=2` =
`WIFI_REASON_AUTH_EXPIRE`. In 5 minutes — over 40 attempts, not one `up:`.

What was checked and ruled out, so as not to guess all over again:

- password (the phone connects with the same password);
- MAC blocking in the router (it does not block);
- PMF: `p 0` changes nothing;
- binding to BSSID: both to the strong AP `cc:d8:43:85:c6:02` and to the weak one;
- `erase_flash`: helped temporarily, then everything came back;
- the interrupt mask in the HX711 (`G 0`) and moving WiFi to Core 1 — not the cause.

### Cause

`wifi_country_t.max_tx_power = 8.5` **had no effect**. Verified by reading it back:

```
W (2572) web: TX power set 8.0 dBm -> ESP_OK, reads back 8 dBm      <-- after the fix
```

before the fix the getter returned **80** — a value physically impossible for a
transmitter, i.e. there was no limit at all, and the radio ran at full power.

This is exactly the case described in the ESP32 community: an overdriven
transmitter makes the access point drop the station right at the authentication
stage (`auth expired` / `association expired`). Nine people confirmed it in the
home-assistant thread; a second cause is there too — a loosened antenna connector.

### The fix

The power limit is set explicitly **after** `esp_wifi_start()` and is immediately
verified by reading it back:

```c
esp_err_t perr = esp_wifi_set_max_tx_power(8);
int8_t tx_dbm = 0;
esp_err_t gerr = esp_wifi_get_max_tx_power(&tx_dbm);
if (gerr != ESP_OK || tx_dbm != 8) {
    ESP_LOGE(TAG, "TX power limit NOT in force (wanted 8, got %d)", tx_dbm);
}
```

An API peculiarity: the setter takes dBm, and in this version the getter also
returns dBm (asked for 8 — got 8). It must be checked: the value from
`wifi_country_t` was silently not applied.

### Result

| | Before | After |
|---|---|---|
| Connection | does not connect, >40 attempts | `rssi: -70…-72`, `up:` |
| Drops over 200 s | 10 in the first 75 s | **0** |
| Fallback to access point | constantly | **0** |

### Also fixed as a side effect: CPU frequency

`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ` was **160** while the S3 maximum is 240, which
is unacceptable for WiFi. Raised to 240 and pinned in `sdkconfig.defaults` with a comment.

### Also fixed as a side effect: the driver restart cycle

Previously the task every 30 seconds did `esp_wifi_stop()` → `esp_wifi_start()`
in order to get back from access point mode to station. If the router refuses,
that is dozens of full PHY restarts per hour, and it was observed that after them
the radio goes deaf (`ap_num=0`). Now: the task runs once every 5 minutes and only
when the device really did stay in access point mode; if the station has already
connected, the access point is never brought up again; the reconnection backoff
is increased from 8 to 20 seconds, so as not to hammer the router.


## The web interface would not open: lwIP was starving (solved 2026-10-09)

### Symptom

`/api/weight` responded in **0.046 s** (185 bytes), while `/` would not open at
all. Checked and ruled out, so as not to search all over again:

- the page is served, but the body arrives empty; `ERR_ADDRESS_UNREACHABLE` in the browser;
- browser cache — added `Cache-Control: no-store, no-cache, must-revalidate`,
  `Pragma: no-cache`, `Expires: 0`, and the headers were confirmed in the response;
- page generator — `add_custom_command` instead of `execute_process`, now an
  incremental build regenerates the page too;
- free heap: **8.5 MB**, a memory limit is ruled out;
- HX711 interrupt mask: **measured 267 µs** (maximum 402) — as intended;
- transmit power 8 / 13 / 17 / 19 dBm — no difference whatsoever;
- not a single connection drop during the whole diagnosis.

### The observation that settled everything

Measurement with a raw socket, without `curl`:

```
total received: 1525 bytes
chunks (seconds, bytes): [(0.02, 85), (0.02, 1440)]
```

85 bytes is the response headers. 1440 is exactly `CONFIG_LWIP_TCP_MSS`. **After
that — silence.** One segment and a stop: as much as fit into the send buffer was
sent, and the buffer never emptied once.

A non-empty buffer on a live connection means exactly one thing: **the
acknowledgements from the receiver are not being processed**. The network stack is starving.

### Cause

`CONFIG_LWIP_TCPIP_TASK_AFFINITY` was `NO_AFFINITY`, and the lwIP thread landed
on Core 0 — the same core where the HX711 measurement task lives, which wakes up
continuously (the DOUT wait polls the line). The lwIP priority is below the
measurement priority, so acknowledgements were not processed, the window did not
move, and transmission broke off at 8192–10240 bytes with `ESP_ERR_HTTPD_RESP_SEND`.

Small responses passed for the same reason: they fit into the buffer entirely and
required no acknowledgement processing at all.

### The fixes

1. `CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU1=y` — the network stack on Core 1
   together with WiFi, the measurement task on Core 0. Each core has one owner.
2. `CONFIG_FREERTOS_HZ` 100 → **1000**: with a 10 ms tick lwIP has too coarse a
   resolution for the retransmission and delayed-acknowledgement timers.
3. `CONFIG_LWIP_TCP_SND_BUF_DEFAULT` and `TCP_WND_DEFAULT` 5760 → **17440**:
   too few bytes in flight for a long transfer.
4. The page is compressed on embedding: 18 544 → **12 830 bytes**.
5. Cache-disabling headers — see above.
6. Dropping chunked encoding: `curl` printed
   `Leftovers after chunking: 1351 bytes` and discarded the body entirely. By
   sending a single body with `Content-Length` the two sides cannot disagree.

### Result

| | Before | After |
|---|---|---|
| `/` | 0 bytes, timeout | **12 830 bytes in 0.96–3.75 s** |
| `/api/weight` | 0.046 s | 0.046 s |
| Stop point | 8192 bytes (one MSS) | not interrupted |

### The general lesson

The symptom looked like "WiFi does not work" and "bad markup". In reality it was
a priority arrangement between the cores. The diagnosis went from symptom to cause
incorrectly: several times I checked things that definitely work — the password,
the power, the cache, the memory. Only the measurement of "how many bytes arrived
per round" helped, which showed exactly `CONFIG_LWIP_TCP_MSS`.


## Accuracy measured by the paired method (2026-10-10, repeated)

Method: the empty platform is taken as a separate point, the reference is placed
separately, and the delta of counts is computed, not the instrument's reading.

**Two independent pairs:**

| Pair | Empty platform | Reference 73.03 g | Delta | Error |
|---|---|---|---|---|
| 5 minutes | 419 425 | 450 569 | 31 144 | **+0.147 g** |
| Alternating | 419 342 | 450 482 | 31 140 | **+0.138 g** |

**The discrepancy between the pairs is 9 mg.** The error is reproducible.

| | |
|---|---|
| Result | **+0.14 g (+0.20 %)** |
| device sigma | 15-18 mg |
| settling walk-off | +0.013 g over 5 minutes |
| disturbed measurements | 0 of 99 |

### Correction of the earlier entry

Previously this said that "two measurements differed by 0.24 g, the spread is
larger than the error". **That was wrong.** One of those two measurements was
spoiled: the reference was placed right in the middle of the capture, and a load I
did not filter landed in the tail. The clean one gave +0.147 g, alternating +0.138 g.

⚠️ **The technique that produced this.** I was discarding outliers with the
threshold `max(5*sigma, 0.15 g)`, which threw away 48 measurements out of 78 —
that is, almost everything, and declared the remainder "accurate". Filtering has
to use the sign the device itself reports: normal `avg_sigma_mg` = 15-18, disturbed = 140 and above.

### What remains the limiter

Not the device: its own uncertainty of 15 mg.

**The empty point wanders between sessions by 195 mg** (419 425 against 419 342).
That, not the noise and not the settling, gives the final figure its uncertainty.


## The averaging window was chosen from the measured knee (2026-10-09)

The request was "widen the window by the smallest possible amount". First it was
measured what the window actually buys — otherwise this is another blind constant edit.

Clean capture of 507 counts, empty platform, σ of a single count 110 mg:

| Window | σ of the mean | Price |
|---|---|---|
| 4.4 s | 30 mg | |
| 8.9 s | 23 mg | it was here |
| **17.8 s** | **15 mg** | **wait twice as long — chosen** |
| 26.6 s | 10.5 mg | wait three times as long |

The measured value is **2–3 times worse** than the `1/√N` prediction for white
noise. That means the noise is correlated in time, and averaging will not get you
far. The knee is at 18 s: going from 9 → 18 s gives back 35 % error for double the
waiting; beyond that each further doubling gives back less than half.

### The freeze had to be retuned for the new window

The step of the mean across the window is comparable to the tolerance, so growing
the window breaks the freeze: the quiet counter resets and the digit never settles.

| | was | became |
|---|---|---|
| `AVG_N` | 6 (9 s) | **12 (18 s)** |
| `LOCK_HOLD` | 4 | **3** |
| `LOCK_TOL` | 0.02 g | **0.03 g** |

### Check on real noise

The capture was repeated up to 6.7 min, load 73.03 g, the device model filters at
11.3 Hz with web polling every 1.5 s:

| | freeze | window σ | distinct values on the screen |
|---|---|---|---|
| `AVG_N=6` | 43 s | 54.9 mg | 1 |
| `AVG_N=12` | **29 s** | **38.4 mg** | 1 |

The new configuration turns out to be **both faster and more accurate**: the gain
from the tolerance and fewer polls outweighs the loss from the window length.

⚠️ **This is a run, not a measurement on the hardware.** The 29 s settling time on
a real weight is not confirmed — a run with the reference in hand is needed.

### What the window does not buy

A trustworthy last digit of 0.01 g. With a mean σ of 15 mg it is still swamped by
noise. What holds the number is the freeze, not the window length. The honest
resolution of the device remains **0.05 g**.



## Accuracy measured by the paired method (2026-10-08)

Weight **73.03 g**, two points taken back to back, a gap of about a minute:

| | Counts |
|---|---|
| Empty platform (reference point) | 419 006 |
| With the weight | 450 103 |
| Delta | 31 097 |

```
mass     73.066 g    reference 73.03 g
ERROR  +0.036 g  (+0.05 %)
```

**`cal_factor` = 425.6 is confirmed.** Proposed from this measurement — 425.81, a
discrepancy of 0.05 %, which is within the noise of a single point. I did not
write it: there is no gain, and the NVS gets rewritten for nothing.

⚠️ **Not re-verified after 2026-10-10.** At that time, with the reference on the
platform, the device showed 73.4 g against 73.03 g, that is **+0.4 g** — six
times more than the stated ±0.07 g. Three errors were fixed at the same time (see
the next section), among them the auto-zero anchor, which added +293 mg to every
weight. After the fixes the reading has **not been re-measured**: the tare in NVS
was taken in the previous session, and taking it afresh needs an empty platform,
i.e. the user's own hands. **The earlier accuracy figures are still unconfirmed.**

### Why the two previous measurements gave +1.0 g and +1.4 %

Both points were taken at different moments, minutes apart, and the platform was
touched in between — the spread of rest between the points is exactly the magnitude
of the discrepancy (measured later: ±0.11 g untouched). A delta of 31 505 counts
with an empty platform read 350 counts lower gives exactly 73.09 g at 425.6.

**A conclusion worth remembering: an error of 1 g on this device is explained
entirely by the methodology, not by the hardware.** That is why I did not write the
previous `cal_factor` — otherwise a foreign error would have landed in the NVS.

### What really limits accuracy

| Parameter | Value |
|---|---|
| Uncertainty with correct methodology | **±0.04…0.07 g** |
| Noise of a single point (σ) | 103 mg — the main limiter |
| Zero walk-off over 10 min | −3 counts, trend +3 mg/min |
| Rate | 11.29 Hz steadily |
| Read failures / WDT | 0 / 0 |

The 0.01 g target from the specification is unreachable not because of the hardware:
with σ = 103 mg a tolerance of ±5 mg would require 40 seconds of averaging. The
drift considered the main limiter does not exist — see "There is no drift".

### Weighing procedure

1. Let the platform settle and do not touch it.
2. Take the tare.
3. Weigh.

The tare must not be taken early (the measured error is 1.8 g), and the
calibration points must not be spread minutes apart.


## Noise bursts are mechanical settling, not a breakdown (2026-10-10)

We were working out why the signal is sometimes stable and sometimes falls apart. Investigated and closed.

### What the signal shape showed

Capture of raw counts, 507 samples, platform with the reference:

| Counts | Median | Spread |
|---|---|---|
| 0-100 | ~450 000 | 6 000-12 000 |
| 150-199 | transition, values from 418 608 to 461 709 | 43 101 |
| 200-507 | **418 900, spread 71-234** | 71-234 |

The same in other captures: the first 350 counts are noisy, after that the spread is 163-238.

**After the platform settles down, the signal becomes very clean.** That means
this is settling after a disturbance, not electrical interference.

### Check on a motionless platform

| | |
|---|---|
| sigma | **112 mg** |
| spread | 754 mg |
| median by sixths | 419 333 -> 419 393 |

**The electronics are fine.**

### What was ruled out by measurement, not by reasoning

| Hypothesis | How it was checked | Outcome |
|---|---|---|
| The supply sags | Multimeter: module 3.26 V, board 3.27 V, stable | **disproved** |
| Interference from our own WiFi | Radio switched off (`x 1`), sigma still up to 3180 mg | **disproved** |
| Bad antenna connector | The antenna is a printed trace, there is no connector | **nothing to check** |
| Corrupted reads | In clean captures not a single `-1` or `0x3FFFFF` | **disproved** |

⚠️ **A multimeter between A+ and A- does not solve this task.** The expected magnitude
for 73 g is about 80 µV, the measured change is 10 µV, that is one count of the display.
The uncertainty of the instrument itself at the 0.03 mV level is comparable.
**An oscilloscope is needed**, one that sees both the ripple of the excitation and the signal.

### Practical conclusion

After pressing "zero", the platform must be **left alone and allowed to settle**.
The zero holds as long as there are no disturbances.


## Three silent errors found on 2026-10-10

None of the three produced an error or a red indicator — only wrong numbers.

### 1. The tare at boot was zeroing the load

Reboot with the reference on the platform: the device took the tare at the level
**450 023** and confidently displayed **0.000 g**. The load was silently swallowed.

Now the auto-tare is checked against the user's stored tare:

| | |
|---|---|
| Tolerance | **851 counts ≈ 2 g** |
| Measured rest spread | 0.6 g spread |
| Headroom | roughly eight times |

An empty platform will never be taken for a load, and any real reference will
remain untouched. If there is no tare in the NVS there is nothing to compare
against, and the tare is taken as before.

```
W weight: NOT taring at boot: platform reads 31191 counts from the stored
tare 419002, which is 73 g - looks like a load is on it.
```

⚠️ **A silently wrong number is more dangerous than a visibly wrong one.** A zero
instead of 73 g is impossible to notice, whereas −9.8 g will ask for a tare by
itself. This principle was chosen deliberately, and not by residual principle.

### 2. `cal_factor` was lost on every reboot

It was written with `nvs_set_blob` and read with `nvs_get_i32`. Reading a blob as
an integer always gives a type error, so every boot printed
`cal_factor missing or invalid` and substituted the built-in 425.60.

**Calibration with the `m` command was written, applied, and silently lost on the
next reboot.** Now it is read back with the same type it is written with.

### 3. The auto-zero anchor did not match the tare from NVS

```c
scale_filter_init(&filter, 425.6f, 418857);   /* azt_anchor = 418857 */
nvs_load(&filter);                             /* tare_offset = 419002 */
```

`scale_filter_init` sets `azt_anchor` from the value passed in, and the task
passed the hardcoded 418857. The real tare was loaded into `tare_offset`, and the
anchor stayed on the built-in one. **Every reading after boot got an offset equal
to the difference of the two tares**, and it grows as the stored tare drifts away.

Measured from outside: anchor 418 878 against a tare of 419 002 = **+293 mg added
to every weight**. After the fix — 16 counts = 37 mg, within the noise.

**How to check the anchor from outside** (useful after any filter change):

```python
anchor = counts - value * cal_factor     # must match tare_offset
```


## Gotchas already stepped on

1. **The HX711 requires a 25th SCK pulse** after the 24 data bits. Without it the
   chip stays in the reading state, DOUT does not go high, and the next read
   returns garbage. Symptom: every second count is `-1`, the rate is half. Easy to
   mistake for a wiring fault. Diagnosis: the garbage arrived exactly after
   1043 µs — the transfer time of 24 bits. **Clarification 2026-10-08:** the same
   symptom (`-1`, `0x3FFFFF`) is caused not only by a forgotten pulse in the
   code. The chip can **lose count of its own clocks** if the SCK period became
   indeterminate — see the section on the interrupt mask. The driver was written
   correctly and issued exactly 25 clocks; the interrupt was to blame.
2. **The CR1 register does not exist.** The old code sent 8 "CR1 bits", that is
   8 extra clocks = 33 per period instead of 25–27, exactly the error case the
   datasheet warns about. Removed completely.
3. **In ESP-IDF v6 the GPIO driver moved** into the `esp_driver_gpio` component,
   so `main/CMakeLists.txt` needs `REQUIRES esp_driver_gpio`. The include itself
   stays `driver/gpio.h`.
4. **The port is a CH9102 USB-UART, not native S3 USB.** Console and flashing go
   through UART0 (GPIO43/44). Native USB (VID `303a`) is a separate device.
5. **The first build requires `idf.py set-target esp32s3`.**
6. **The mass of a reference must not be taken from memory.** I took 5 coins of 5 rubles
   for 15 g and got `cal_factor` wrong by a factor of two (871 instead of 425). The real
   weight is 30.63 g. The device was right twice, I was not. **The second reference in the same session caught the error.**
7. **A 2-fold discrepancy at first looks like a hardware fault.** I built
   theories about "the platform rests on the table" and "the load cell sticking"
   without rechecking my own arithmetic. Recalculate your numbers first, then look for a fault.
8. **Measurements from different days cannot be compared directly.** The tare "yesterday"
   of 418 602 and "today" of 418 815 are different drifts. Take all points of one calibration back to back.
9. **`esp_wifi_set_config` for the access point goes through `WIFI_IF_AP`.**
   Through `WIFI_IF_STA` it returns `ESP_ERR_WIFI_MODE`, and five retries with a pause
   do not help — change the mode to `WIFI_MODE_NULL` and wait.
10. **In ESP-IDF v6 the HTTP client needs `perform()`.** Without it `read()` will
    return 0 bytes and status 0, even if the server is alive: the request was never sent.
11. **`ESP_ERROR_CHECK` in the failure handler = a reset loop.** The access point
    crashed inside `ap_start()`, and the board rebooted every 16 seconds. Recovery
    handlers must not call abort.
12. **The WiFi scan does not start while the station is authenticating.** A flag is
    needed that the reconnection handler looks at, otherwise `ESP_ERR_WIFI_STATE`.

## Known risks of stages 3–5

- **GPIO3 is a strapping pin** (missing from the list in the specification). Assigned
  to BTN_UNIT with INPUT_PULLUP; it is pulled high on reset. Move it to a free pin.
- **The reserved GPIOs are broader than the specification says**: with Octal PSRAM,
  26–37 are occupied, not only 35–37.
- **The LVGL buffer must not be in PSRAM** — SPI DMA on the S3 cannot read from
  there. An internal DMA-compatible buffer is needed.

## Not established

## Target accuracy: 0.05 g (decided 2026-10-07)

**The specified 0.01 g from the specification is officially dropped.** The target
accuracy is **0.05 g at rest, 0.1 g in motion**. No soldering, 11.3 Hz.

Reason: with σ = 106 mg a tolerance of ±5 mg would require 448 counts = 40
seconds of averaging. At α=0.05..0.10 the filtered noise is 12.7–21.9 mg, which
fits inside 0.05 g, and a settling of 2.7 s is acceptable for a scale.

## What must be recalculated from the specification for 0.05 g

The numbers in the spec are written for a 0.01 g resolution and become harmful at 0.05 g:

| Spec parameter | Value | What is wrong at 0.05 g |
|---|---|---|
| Deadband | ±0.03 g | Swallows more than half of the displayed digit |
| AZT window | ±0.05 g in 2 s | Triggers in normal use |
| STABLE indicator | σ < 0.02 g over 10 counts | Always "stable", because it is computed at the EMA output |
| Display resolution | 0.01 g | Must become 0.05 g |

**The AZT and deadband thresholds must be derived from the measured noise, not taken from the spec.**

Derived from σ=45 counts at α=0.10 (a window of 9 counts, σ_filter = 9.3 counts):

| Parameter | Spec value | Value from the noise | Why |
|---|---|---|---|
| Deadband | ±0.03 g | **±0.066 g** (3σ_filter) | The spec gives 0.28σ — too tight, the noise will not be cut |
| AZT window | ±0.05 g / 2 s | **±0.11 g** (5σ_filter) | The spec gives 0.47σ, AZT will trigger for nothing |
| STABLE threshold | σ<0.02 g | **σ<0.21 g** over 10 raw counts (2σ) | Compute **before** EMA |

Additionally, from the earlier analysis of the spec: the STABLE indicator must be
computed **before** EMA (on raw or median counts), otherwise it is always lit. AZT
must be disabled at step 1 of the calibration master, or it swallows the true zero.
- **Calibration weights for production calibration.** Buy a set of class 0.1% (a
  handful of 100 g pieces — pennies). The pharmacy scale gives 0.01% but tops out
  at 200 g = 3.3% of range; the kitchen scale covers 1327 g = 22%, but its real
  uncertainty measures as ±1 g. **While there are no weights: the pharmacy scale
  sets the scale (161 g), the kitchen scale checks the top (1327 g).** `cal_factor` is unchanged.
- **Check with a multimeter that the module VCC = 3.3 V** under load. 425 counts/g
  theoretically corresponds to an excitation of ~3.3 V, so indirectly everything
  agrees, but nobody has done a direct check. If VCC = 5 V, GPIO16 takes 5 V on DOUT.
- **The GitHub repository is not created** — `gh` is not authorised, the token has
  expired; `gh auth login` is needed. Git identity: `Antigravity AI <antigravity@google.com>`.

## Stage 2: plan (updated after calibration)

We verify the maths offline on `logs/v2_*.log` before the UI appears.

1. **Filter offline**: median N=5 + adaptive EMA + AZT + deadband. Check that it
   gives the calculated 12–35 mg and cuts outliers. Reference points from
   measurements: 161 g → 68 692 counts, 30 g → 13 014, tare → 418 815. **Derive the
   AZT/deadband thresholds from the measured noise**, not from the spec — see above.
2. **FreeRTOS**: Core 0 — measurement and filters, Core 1 — UI, exchange via a queue.
3. **ST7789 display**, buttons, RGB on GPIO 48.
4. **NVS** via Preferences, `magic_key` = 0xAA55BEEF, check `cal_factor == 0`.
5. **Calibration master**, 2 points, entry by holding CAL > 3 s. The defaults are
   filled in from measurements: `tare_offset` 418 815, `cal_factor` 425.2.
