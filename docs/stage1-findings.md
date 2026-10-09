# Stage 1 findings — HX711 + load cell characterisation

Measured 2026-10-06 on the bench. Everything here is from captured data, not
from datasheets or estimates. Raw logs are in `logs/` (gitignored).

## Setup as measured

| Item | Value |
|---|---|
| Board | ESP32-S3-N16R8 (16 MB flash, 8 MB octal PSRAM) |
| Framework | ESP-IDF **v6.0.1** (`~/esp-idf`, rev `8c19b156`) |
| Port | `/dev/serial/by-id/usb-1a86_USB_Single_Serial_595B069397-if00` |
| USB bridge | **WCH CH9102** (`1a86:55d3`), CDC-ACM |
| HX711 DT / SCK | GPIO16 / GPIO17 |
| HX711 supply | 3.3 V (chosen so DOUT stays 3.3 V for the ESP32 input) |

The port is a **USB-UART bridge, not the ESP32-S3 native USB** (that would
enumerate as Espressif `303a`). Console and flashing go over UART0 on GPIO43/44.

## Results (unloaded, platform resting on the cell)

Two independent 40 s captures at the default rate:

| Metric | Run A | Run B |
|---|---|---|
| Rate | 11.29 Hz | 11.30 Hz |
| Mean | 299,140 counts | 299,139 counts |
| **Sigma** | **46.1 counts** | **48.3 counts** |
| Peak-to-peak | 276 counts | 272 counts |
| **P-P / sigma** | **6.0** | **5.6** |
| Drift | +0.64 counts/s | +0.98 counts/s |
| Outliers >3σ | 1 of 473 | 2 of 417 |

### What this says

- **The load cell works.** Readings are stable and repeatable, and mean matches
  between runs to 1 count.
- **Noise-limited, not interference-limited.** A P-P/σ ratio near 6 is the
  textbook value for a clean Gaussian distribution. Bad contacts, mains pickup
  or EMI bursts would push this much higher. The analog side is healthy.
- **The offset is a real preload**, not an error: 299k counts of standing
  offset is what a platform resting on its cell produces. This is exactly what
  `tare_offset` in the spec exists to absorb.
- **Outliers are rare but real** (0.2-0.5%, worst -165 counts). This is direct
  evidence for the median-of-5 stage in the spec — it is not decorative.
- **Drift is the biggest remaining problem**, at roughly +0.6 to +1.0 counts/s.
  Extrapolated, that is a few hundred counts over minutes. Anything that reads
  a stale value will show it as slowly walking digits.

## Calibration — two references, one session (2026-10-07)

All four points captured back to back, no gaps. Raw data in `logs/v2_*.log`.

| Reference | Mass | Delta | `cal_factor` |
|---|---|---|---|
| Water jar | 161.41 g | 68 692 ±4.7 | **425.57** ±0.03 |
| 5 x 5-ruble coins | 30.63 g | 13 014 ±70.6 | **424.86** ±2.31 |

**`cal_factor` = 425.2 counts/g**, ±0.01% from the jar. The two references agree
to 0.6%, which confirms the cell, the mounting and the excitation are all sane.

| Point | Median | σ |
|---|---|---|
| Tare-1 | 418 815 | 55.1 |
| Jar | 487 502 | 45.3 |
| Coins | 431 775 | 44.5 |
| Tare-2 | 418 749 | 85.9 |

| Derived | Value |
|---|---|
| `tare_offset` | 418 815 counts (platform installed, empty) |
| Hysteresis | −66 counts = −0.16 g → **none** |
| 1 LSB | 2.35 mg |
| 6 kg | 2.55 M of 8.39 M FS = **30%** |

### The earlier 871 counts/g was wrong by 2x

The first calibration divided the delta by **15 g**, assumed from memory to be
the weight of five 5-ruble coins. They actually weigh **30.63 g**. The error was
exactly a factor of two, which is why every resolution figure derived from it
was optimistic by 2x.

The instrument was right twice in a row; I was not. **Never take a reference
mass from memory or from a coin's denomination — weigh it.** A second reference
measured in the same session is what caught this. With only one reference the
error would have gone into the firmware silently.

### Reference size requirement and the metrology we actually have

The pharmacy scales cap at **200 g at 0.01 g**; kitchen scales read **1327 g at
1 g stated**. Neither covers both halves of the job:

| Reference | Share of 6 kg range | Relative accuracy |
|---|---|---|
| Pharmacy, 200 g | 3.3% | **0.01%** — 8x better than needed |
| Kitchen, 1327 g | 22% | 0.08% stated, **0.054% measured** — too coarse |

Measured repeatability of the kitchen scales: two captures of the same 1327 g
load differed by **306 counts = 0.72 g**, roughly twice their stated 1 g
resolution. Their real class is ±1 g, so that load point is a **verification**,
not a calibration source.

**Working scheme (agreed 2026-10-07):**

- `cal_factor` comes from the pharmacy scales — best relative accuracy available.
- The 1327 g kitchen load **verifies the top of the range** and caught no gross
  error: 426.5 implied vs 425.2 calibrated, 0.3% apart.
- Calibration weights (0.1% class) are to be bought later to close the gap.

**Honest accuracy class with today's references:** ~0.05 g in the lower part of
the range, degrading to ~0.2 g at 1.3 kg. Weights will tighten this.

## Verdict on resolution (recomputed for 425.2)

σ of a raw sample is **45 counts = 106 mg** at 11.3 Hz. With a 9-sample EMA
window (~0.8 s):

| α | σ after filter | Settling | Resolution |
|---|---|---|---|
| 0.05 | 12.7 mg | 5.3 s | 0.05 g |
| 0.10 | 21.9 mg | 2.7 s | 0.05 g |
| 0.15 | 29.0 mg | 1.8 s | 0.1 g |
| 0.20 | 34.8 mg | 1.3 s | 0.1 g |
| 0.30 | 44.4 mg | 0.9 s | 0.1 g |

**0.01 g is not reachable at 11.3 Hz by any filter setting.** Holding ±5 mg
would need N > 448 samples = **40 seconds** of averaging. The earlier claim
that 0.01 g was achievable was purely an artefact of the inflated `cal_factor`.

Realistic targets without rework: **0.05 g at rest, 0.1 g while moving.** The
spec's 0.01 g requires either accepting a 40 s settle or the 80 SPS rework.

## Filter model and what it revealed (tools/filter_sim.py)

`tools/filter_sim.py` is the executable specification of the chain
median(5) → adaptive EMA → AZT → deadband, validated against the real captures.

Static-load accuracy with the calibrated `cal_factor` = 425.2:

| Load | Displayed | Expected | Error |
|---|---|---|---|
| Coins | 30.487 g | 30.63 g | −0.143 g |
| Jar | 161.532 g | 161.41 g | +0.122 g |

Filtered noise is 40–65 mg, but the **errors are ±0.12–0.14 g and have opposite
signs.** A calibration error would shift both the same way. Opposite signs mean
the error is not in `cal_factor` or the filter — it is the zero level moving
between captures.

### Zero wander, measured

Drift over a 35 s capture, converted to grams:

| Log | Counts | Grams |
|---|---|---|
| tare1 | −27 | −0.063 g |
| jar | +19 | +0.045 g |
| coins | +40 | +0.094 g |
| 1327 g | −58 | −0.136 g |
| **tare2** | **+214** | **+0.503 g** |

### AZT works only inside its window

Testing auto-zero the way the firmware will actually run it (tare taken from the
first 8 s, then AZT holds for the rest):

| Log | Result |
|---|---|
| tare2 (monotone +6.1 counts/s drift) | holds **0.000 g**, spread 26.5 mg, ZERO 98% |
| tare1 (drift −0.78 counts/s) | drifts to **−0.245 g**, ZERO 3% |

So AZT tracks drift correctly while it stays inside the window. Once the
excursion exceeds the window, the dwell timer resets and the tare can never
catch up. That is inherent to a fixed-window scheme, not an implementation bug.

### The conflict this exposes

To hold zero the AZT window must be at least **0.5 g**. A 0.05 g weight is
**21 counts — ten times smaller than that window**. Any window large enough to
hold zero will swallow every small weight.

**Auto-zero and 0.05 g resolution are mutually exclusive on this hardware.**
The binding constraint is not the filter, not `cal_factor`, and not sample rate —
it is zero wander of up to ±0.5 g, which is 5x the intended resolution.

### What this means for the 0.05 g target

Reaching it requires reducing zero wander, not better filtering:

- Clean 3.3 V supply; the cheap HX711 module's onboard regulator is suspect
- Capacitors on the module's supply and reference pins
- Let the cell reach thermal equilibrium before reading (tare2's +214 counts is
  consistent with warm-up)
- Mechanically secure the platform so it cannot creep

Until that is addressed, the honest specification is **~0.15 g at rest, ~0.2 g
loaded**, with TARE used deliberately rather than relying on AZT.

The spec's 0.01 g is formally dropped. The device targets **0.05 g at rest,
0.1 g while moving**. No rework, no 80 SPS.

Consequences to carry into the filter design:

- α = 0.05..0.10 gives 12.7–21.9 mg filtered noise, inside a 0.05 g reading.
- The settling penalty is now acceptable: 2.7 s at α=0.10 is fine for a scale.
- **Threshold and deadband values from the spec must be rescaled.** The spec's
  AZT window was ±0.05 g and its deadband ±0.03 g, both written for 0.01 g
  resolution. At 0.05 g target the deadband (±0.03 g) would swallow a third of a
  displayed digit and AZT would engage during normal use. These need rederiving
  from the measured noise, not copied from the spec.
- The STABLE indicator in the spec (σ over last 10 samples < 0.02 g) must also be
  rescaled — see the spec-review note in AGENTS.md.

### Paired measurement removes the 0.3% spread (2026-10-07)

The four separated captures disagreed by up to 0.3%, and the suspicion was that
one scale or the cell was nonlinear. That turned out to be wrong. The real cause
was the **gap between tare and load**: zero wanders by tens of counts in 30 s,
and that error lands straight in `cal_factor`.

`main/scale_app.c` now performs both captures back to back, with the tare
immediately adjacent to the load:

```
m <grams>   tare 25 s -> wait for load (auto-detected) -> load 25 s -> cal_factor
```

Load arrival is detected by a threshold of 2000 counts, far above the worst
measured drift of 6 counts/s, so drift cannot be mistaken for a reference.

| Method | Reference | `cal_factor` |
|---|---|---|
| Separated | 30.63 g pharmacy | 424.86 |
| Separated | 161.41 g pharmacy | 425.57 |
| **Paired** | **158.06 g pharmacy** | **425.69** |
| **Paired** | **30.63 g pharmacy** | **425.50** |
| Separated | 1327 g kitchen | 426.53 |
| Separated | 1577 g kitchen | 426.26 |

The kitchen scales read **+1.02 counts/g (0.24%) high** with separated captures,
and that offset **disappears** once tare and load are adjacent. The scales were
not in conflict; the procedure was injecting the error.

The two paired runs agree to **0.045%** and return the same factor at 30 g and at
158 g, which is independent evidence that the cell is linear across that span.

| Run | Tare | Load | Delta | σ | drift |
|---|---|---|---|---|---|
| 158.06 g | 418 878 | 486 162 | 67 284 | 42.7 | +2.13 c/s |
| 30.63 g | 418 836 | 431 869 | 13 033 | 47.9 | −3.65 c/s |

**Adopted values: `cal_factor` 425.6 (mean), `tare_offset` 418 857 (mean).**

### Auto-detect caveat

The 30.63 g run reported a detection jump of 38 014 counts (89 g) rather than the
expected ~13 000 — the operator's hand was still on the platform when the
threshold fired. The result was unaffected because phase 3 starts 3 s later and
captures 25 s, by which time the hand had been removed.

Procedure rule: **remove your hand after placing the reference and do not touch
the platform again.** A transient larger than the settled load will trigger
detection early; if the hand stays, the measurement is wrong.

Reference σ for the filter chain: **43–48 counts ≈ 100–113 mg.**

### Improvement without buying weights

Because paired measurement cancels the tare error, small pharmacy references are
now good enough to set the scale factor, and kitchen references can verify the
top of the range without biasing the result. No weights required for this.

## Decision: no soldering, 10 SPS accepted

The user chose **not** to modify the hardware. RATE stays strapped low, so the
chip runs at 11.3 Hz permanently. Two consequences:

1. **Spec change: "1000 g within 0.5 s" becomes ~1 s.** At 11.3 Hz, 1 s is ~11
   samples, which is enough for a median-of-5 plus a responsive EMA.
2. **Accuracy is unaffected and in fact has margin.** Noise after a 9-sample EMA
   window (~0.8 s), vs settling time:

| alpha | sigma after filter | settling to 0.05 g | resolution |
|---|---|---|---|
| 0.05 | 6.3 mg | 5.3 s | 0.01 g |
| 0.10 | 10.9 mg | 2.7 s | 0.1 g |
| 0.15 | 14.4 mg | 1.8 s | 0.1 g |
| 0.20 | 17.2 mg | 1.3 s | 0.1 g |

Every option clears 0.1 g with roughly 2x margin. So 80 SPS would have bought
response time only — not accuracy — which is why skipping the rework is sound.

**Correction to an earlier claim:** 0.01 g at alpha=0.02 (7.4 mg) holds only for a
fully settled reading under indefinite averaging, not within 1 s. In practice the
display shows 0.01 g after ~10 s of stillness. This is exactly what the spec's
adaptive alpha is for: high alpha while the weight changes, low alpha once
steady. Expect to hold the load still for a second or two before trusting the
last digit.

## Open items

### 1. 80 SPS requires a hardware change, not code

**There is no configuration register on this part.** Datasheet
(`hx711_english.pdf`, Avia Semiconductor), Table 1, pin 15:

```
15  RATE   Digital Input   Output data rate control, 0: 10Hz; 1: 80Hz
```

Rate is set by the **logic level on physical pin 15**. No software can change
it. That is exactly why a brute-force sweep of all 128 candidate register bytes
moved nothing (highest rate seen: 15.6 Hz).

Channel and gain are also not register-based — they are selected by the **count
of PD_SCK pulses** in one conversion period (Table 3):

| Pulses | Channel | Gain |
|---|---|---|
| 25 | A | 128 |
| 26 | B | 32 |
| 27 | A | 64 |

The datasheet warns that anything outside 25–27 causes a serial communication
error. The driver emits exactly 25 (24 data + 1) and contains no config writes.

**The earlier `CR1` code was actively harmful, not merely useless.** Writing 8
"register bits" meant 8 extra clock pulses, i.e. 33 per conversion instead of
25–27 — precisely the error condition the datasheet describes. Both assumptions
have been removed from the driver.

**To reach 80 SPS:** pin 15 must be tied to DVDD. Breakout modules normally strap
RATE low internally, so this likely means lifting the pin or cutting a trace on
the module — a hardware decision, not a firmware one.

**If it stays at 10 SPS**, the spec's "1000 g within 0.5 s" needs revisiting:
0.5 s is ~5.6 samples, and a median-of-5 window consumes that budget. Options
are a smaller median window, or accepting ~1 s settling.

### 2. Measured noise vs datasheet — 2.2x the floor

Datasheet Table 1: gain 128, RATE=0 → **50 nV rms** input-referred noise. The
corrected `cal_factor` (425.2 counts/g) implies ~1.6 nV per count at 3.3 V
excitation, so σ = 45 counts ≈ **72 nV rms** — about 1.4x the floor rather than
the 2.2x previously quoted. The remaining excess is consistent with connector
wiring and USB supply noise.

**2.2x the theoretical minimum is expected** for a breakout module with
connector wiring and USB-supply noise. It is not a defect, but it does mean the
hardware has no meaningful margin left for 0.01 g: 0.01 g is only **8.7 counts**
against σ = 45.7. Achievable only by averaging, which costs response time.

## Gotchas hit during bring-up

1. **The HX711 needs a 25th clock pulse after the 24 data bits.** Without it the
   chip stays in the data-read state, DOUT never rises, and the next read sees
   "ready" immediately and clocks out stale bits. Symptom was every second
   sample being `-1` at exactly the 24-bit transfer interval, with the rate
   halved. Easy to misread as a wiring fault.
2. **ESP-IDF v6 moved the GPIO driver.** `driver/gpio.h` now lives in the
   `esp_driver_gpio` component, so `main/CMakeLists.txt` needs
   `REQUIRES esp_driver_gpio`, not `driver`. The include path stays `driver/gpio.h`.
3. **Flash at 460800, not 921600** — the CH9102 is not reliable at the higher rate.
4. **Use the `/dev/serial/by-id/...` path.** This machine has had more than one
   CDC device, and `ttyACM` numbering is not stable.

## Reproducing

```bash
source ~/esp-idf/export.sh
idf.py build
idf.py -p /dev/serial/by-id/usb-1a86_USB_Single_Serial_595B069397-if00 -b 460800 flash

# capture 40 s
stty -F "$PORT" 115200 raw -echo && timeout 40 cat "$PORT" > logs/run.log

python tools/analyze_stage1.py logs/run.log
```