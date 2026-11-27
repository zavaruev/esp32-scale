### Display behaviour in v1.4.0

![ST7789 wiring diagram](docs/images/wiring-st7789.svg)

* **Units.** `ED` toggles between AUTO and ounces. AUTO shows grams with two
  decimals below 1 kg and kilograms with three decimals at or above 1 kg. The
  web page and the panel use the same averaged reading for that decision, so
  they cannot disagree at the boundary. Since v1.4.3 the web page offers the
  same two buttons (AUTO / oz) and highlights the *selected mode* from the new
  `unit_mode` field in `/api/weight` (3=AUTO, 2=oz); previously it still showed
  three g/kg/oz buttons and took the highlight from the effective unit, so AUTO
  looked like a manual g or kg selection.
* **Buttons.** `SUM` starts calibration (short or long), `ITT` holds/releases
  the result, `SBR` clears the hold, `T` takes a tare (long press toggles AZT),
  `0` zeroes the reading.
* **Overload.** Above 6 kg the number turns amber and `! >6 kg` appears - the
  reading keeps updating. At 10 kg the display switches to `OVER` / `PANIC` and
  stops updating. Both thresholds have hysteresis to avoid repeated alarms.
  These are mechanical warnings: the HX711 still has range up to about 19.7 kg,
  but the cell is rated for 6 kg.
* **Idle clock.** After 60 s with an empty, stable platform the panel shows a
  large `HH:MM` and date. Time comes from NTP (`pool.ntp.org`, MSK/UTC+3); it
  displays `--:--` until synced. A load of 5 g or more, or any button press,
  returns immediately to the weighing screen; smaller loads must persist for
  about 4 s.
* **Colours.** The panel applies BGR channel order and inversion. The
  framebuffer colours in `main/ui.c` are pre-distorted so the displayed colours
  are the intended dark blue, white, blue and green.

![Web interface](docs/images/web-ui.png)

