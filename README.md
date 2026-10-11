# ESP32 Scale

High-precision electronic scales on an ESP32-S3: a 6 kg load cell, an HX711
converter, custom firmware on ESP-IDF and a web interface.

Nothing here comes from ESPHome, Arduino or any off-the-shelf measurement
library — ESP-IDF and the code in this repository only.

## What the instrument actually shows

Not the numbers from the specification, but measured ones. The method is a
paired measurement: the empty platform is captured as its own point, the
reference is placed separately, and the difference in counts is what gets
computed.

| Parameter | Value | How it was obtained |
|---|---|---|
| Sampling rate | **11.3 Hz** | limited by the hardware, see below |
| Noise of one sample | **110 mg** | 507 samples on a motionless platform |
| Noise of the device-side mean | **15 mg** | mean over 45 samples = 4.0 s |
| Accuracy on a 73.03 g reference | **+0.14 g (+0.20 %)** | two independent pairs, 9 mg apart |
| Time to a reading | **immediate** | the mean is computed in the device, no network wait |

One honest caveat: the empty-platform point wanders by **195 mg** between
sessions. That is the real limiter of accuracy, not the instrument, whose own
uncertainty is 15 mg. Hence: **tare before every weighing.**

## Hardware and wiring

HX711 to the ESP32-S3:

| HX711 | ESP32-S3 | Note |
|---|---|---|
| VCC | **3V3** | the HX711 drives DOUT at its own supply level; on 5 V that puts 5 V on GPIO16 |
| GND | GND | |
| DOUT | GPIO16 | |
| SCK | GPIO17 | |

6 kg load cell:

| Wire | Goes to |
|---|---|
| black | E+ |
| red | E− |
| green | A− |
| white | A+ |
| yellow | shield to GND |

**Place the load dead centre on the platform.**

### Why the rate is not higher than 11.3 Hz

The HX711 has no configuration register. The rate is set by the level on pin
15: 0 for 10 Hz, 1 for 80 Hz. Pin 15 is tied to ground, so the instrument runs
at 11.3 Hz permanently. Rewiring it would give 80 SPS but would not improve
accuracy: accuracy is set by the noise and by the length of the averaging
window, not by the sampling rate.

## Build and flash

```bash
source ~/esp-idf/export.sh
cd ~/ESP32_Scale
idf.py set-target esp32s3      # first build only
idf.py build

PORT=/dev/serial/by-id/usb-1a86_USB_Single_Serial_595B069397-if00
idf.py -p "$PORT" -b 460800 flash
```

**460800, not 921600** — on the CH9102 anything faster is unreliable.

Before the first flash, create the WiFi settings file:

```bash
cp main/credentials.h.example main/credentials.h
# fill in SSID and password
```

`main/credentials.h` is in `.gitignore` and never reaches the repository.
`main/credentials.h.example` is the only such file that is tracked.

## Web interface

The device joins the `home4` access point and takes an address over DHCP, at
the moment of writing `http://192.168.22.123/`.

- The page is served gzip-compressed: **4 073 bytes** instead of 12 491.
- Every 1500 ms the device is polled and the reading updates.
- **Zero** is the tare button.
- **Auto-zero** turns the zero-tracking on and off.
- The moon/sun button in the top bar switches between day and night. The choice
  is kept in `localStorage`, so it belongs to the browser and to this origin: a
  different DHCP lease on the instrument loses it and the page falls back to the
  system preference.

If the device is connected but the address is not that one, look at the serial
console — it prints the address it was given.

**There is no access-point fallback at the moment** — it is disabled on
purpose, by request. The device acts as a station only.

## Serial console

115200 baud. The useful commands:

| Command | Action |
|---|---|
| `c` | current reading, noise, zero and stability flags |
| `t` | tare |
| `d [min]` | watch zero stability |
| `m [g]` | paired calibration with automatic load detection |
| `w` | filter constants and state |
| `R` | dump raw samples recorded by the measurement task |
| `h` | HTTP server self-test |
| `x 0\|1` | suspend / resume the WiFi radio |
| `T <dBm>` | transmit power at runtime |
| `G 0\|1` | interrupt mask around the HX711 read |
| `s` | WiFi scan: SSID, RSSI, channel, encryption |
| `k <password>` | replace the password without rebuilding |
| `b <MAC>` | bind to a specific access point, `b off` to clear |
| `p 0\|1`, `a 0..2` | PMF and authentication mode |
| `L 0\|1`, `M <MAC>` | WiFi debug log and MAC override |

For live output instead of redirecting to a file:

```bash
python3 tools/serial_console.py --send 'R' --seconds 50
```

## HTTP API

| Method and path | Purpose |
|---|---|
| `GET /` | the page |
| `GET /api/weight` | current reading |
| `POST /api/tare` | tare |
| `POST /api/unit?u=0\|1\|2` | units: 0 = g, 1 = kg, 2 = oz |
| `POST /api/azt?off` | switch auto-zero off; no parameter switches it on |
| `POST /api/cal/start[?mass=g]` | calibration wizard |
| `GET /api/cal` | calibration progress |

`/api/weight` returns `value`, `grams`, `counts`, `sigma_mg` (noise of a single
sample), plus `avg_grams`, `avg_sigma_mg` and `avg_n` — the device-side mean and
its standard error. **Look at `avg_sigma_mg`: it states outright how far the
figure can be trusted.**

## Code layout

| Module | Role |
|---|---|
| `main/hx711.{c,h}` | HX711 driver, exactly 25 clocks, no configuration writes |
| `main/scale_filter.c` | median(5) → adaptive EMA → auto-zero → deadband |
| `main/weight_task.c` | measurement task on **Core 0**, NVS, mean over 45 samples |
| `main/web_server.c` | WiFi and HTTP on **Core 1** |
| `main/scale_app.c` | initialisation and the serial console |
| `main/web/index.html` | interface source, readable and commented |

The core split is not a style preference but a necessity: **Core 0 does
measurement only, Core 1 does networking and the interface only.** The
measurement task wakes continuously, and with the network stack on the same core
it went hungry and stopped processing TCP acknowledgements — the page would
load 4 500 bytes and then stop.

`main/index_html.h` is generated from `main/web/index.html` by
`tools/embed_web.py` during the build, and is tracked in the repository so a
fresh clone builds without Python. **Edit the HTML, not the header.**

## Known limitations

- **The empty-platform point wanders 195 mg between sessions.** This is the
  limiter of accuracy. Tare before weighing.
- **WiFi is unreliable in this location.** Measured: loss swings between 0 and
  100 % and the link wanders on its own over time. Transmit power has been
  tuned (17 dBm, up from 8), but software cannot fix the physics.
- **A multimeter is not good enough to diagnose the cell.** The expected signal
  for 73 g is about 80 µV, and an ordinary meter reports its own error at that
  level. An oscilloscope is needed.
- **The access-point fallback is disabled.** If the station cannot connect, the
  device will have no web interface. Re-enabled by setting `AP_FALLBACK_ENABLED`
  in `main/web_server.c`.
- **The page and the firmware are coupled.** The interface expects `avg_grams`
  from the API. Flash an old page from a browser cache onto new firmware and the
  figure comes up empty.

## Where the measurement history lives

`AGENTS.md` is the project's working notebook: every measured number, every
mistake that was stepped in, and every rejected hypothesis together with what
disproved it. It also records why the rate is stuck at 11.3 Hz, why the read
needs an interrupt mask, and why averaging is done in the device rather than in
the browser.

`docs/stage1-findings.md` holds the stage 1 results: hardware suitability,
calibration, and calculations of achievable accuracy.
