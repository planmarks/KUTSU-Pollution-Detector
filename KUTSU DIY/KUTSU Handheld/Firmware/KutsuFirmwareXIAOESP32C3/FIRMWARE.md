# KUTSU Sense — Firmware Internals (XIAO ESP32-C3)

[← Handheld firmware](../README.md) · [Hardware](../../HARDWARE.md) · [Setup & wiring](README.md)

Handoff reference for the current firmware. For board-package + library install and the
wiring table, see `README.md`. For all tunables, see `config.h`.

## 1. What this is
An environmental monitor on the **Seeed XIAO ESP32-C3** (Arduino framework). It reads four
quantities, shows them on a 128×64 I²C OLED, charts the last hour, raises a pulsing
full-screen alarm + 3-beep buzzer on harmful readings, logs CSV to MicroSD (flash
fallback), and writes a fixed NFC health record. It also passively scans nearby WiFi
APs (**2.4 GHz only** — the C3 has no 5 GHz radio) and BLE devices on two dedicated screens.

**This build lineage:**
- [`KUTSU PICO`](../../../../KUTSU%20PICO/README.md): the original **RP2040 + CircuitPython** build.
- `KutsuSense/` (this folder): **this** Arduino build, retargeted from the XIAO ESP32-C5. The C3
  shares the XIAO form factor (identical pins) but is 2.4 GHz-only and single-core; see the
  strapping-pin note below.

## 2. Hardware & pins (see `config.h` / `README.md`)
| Function | XIAO pin | Notes |
|---|---|---|
| I²C SDA / SCL | D4 / D5 | shared bus; **needs external 4.7k pull-ups → 3V3** (C3 has none) |
| OLED / HTU21D / CCS811 / NTAG | I²C | 0x3C / 0x40 / 0x5A / 0x55 |
| CCS811 WAK / ADD | → GND | **WAK→GND required** or CCS811 won't init |
| MicroSD (SPI) | SCK D8, MISO D9, MOSI D10, CS D3 | `SPIClass(FSPI)` @ 1 MHz; **D8/D9 = GPIO8/9 strapping pins** (may block boot with a card inserted) |
| Buzzer | D2 | active buzzer (on/off) |
| Button 1 / Button 2 | D6 / D7 | INPUT_PULLUP, active-low |

Sensors/OLED on **3V3**. Common ground.

## 3. Measurements
Live-screen order: **eCO2, TVOC, Temp, Hum**.
| Shown as | Sensor | Unit | Nature |
|---|---|---|---|
| `eCO2` | CCS811 | ppm | **estimated** CO₂, not true NDIR (needs run-in/burn-in) |
| `TVOC` | CCS811 | ppb | concentration estimate (needs run-in/burn-in) |
| `Temp` | HTU21D | °C | calibrated absolute |
| `Hum` | HTU21D | %RH | calibrated absolute |

## 4. Alarm thresholds (`config.h`) — sev 1 = warning `!`, sev 2 = alert (full-screen + beeps)
| Metric | Warning `!` | Alert |
|---|---|---|
| eCO₂ | ≥ 900 ppm | ≥ 2000 ppm |
| TVOC | ≥ 660 ppb | ≥ 3000 ppb |
| Temp | 50–60 °C | < 0 or > 60 °C |
| Humidity | 25–30 or 60–70 %RH | < 25 or > 70 %RH |

## 5. Firmware architecture
Single sketch `KutsuSense.ino` + `config.h`. Top-to-bottom map is in the `.ino` header block.

**Data flow, once per second (`loop()` 1 Hz tick):**
`readSensors()` → `historyAdd()` → `evaluate()` → alarm-pulse-or-`renderScreen()` → `logRow()`.
`updateBuzzer()`, `handleButtons()` and `serviceScans()` run every loop iteration.

**WiFi/BLE (background, non-blocking):** WiFi uses an **async** scan re-kicked every
`WIFI_SCAN_INTERVAL_MS` (5 s) and harvested via `WiFi.scanComplete()` into `apList`
(`harvestWifi`). BLE scans **continuously** via a NimBLE task callback (`ScanCB::onResult →
bleUpsert`) that fills an **aged table** `bleTab` (mutex-guarded; entries expire after
`BLE_STALE_MS`); `bleSnapshot()` (loop thread) prunes it, builds the display list, and writes
the SD log. Distances come from `rssiMeters()` (log-distance path loss; constants in
`config.h`). AP client count is `n/a` (unavailable to a passive scanner). Both scanners also
snapshot to SD (§7).

**Key structures:**
- `Reading` — one tick's values + per-sensor `*Ok` validity flags (invalid = `-1`/`NAN`).
- `Eval` — per-metric severities (0/1/2), `harmful` bool, alarm `msg` + `detail`.

**Key functions:** `readSensors`, `historyAdd`/`metricValue` (4 metrics × 60 one-minute
bins), `evaluate` (thresholds→severity), `showWelcome`/`showLive`/`showCharts`/`showNetworks`/`showDevices`/`showAlarm`,
`harvestWifi`/`bleUpsert`/`bleSnapshot`/`serviceScans`, `logRow`, `nfcProvision`, `handleButtons`.

## 6. UI behavior
- **Boot:** welcome screen ("KUTSU / 6th Sense / By Papathought") → a **boot-status screen**
  that lists each module (OLED / HTU21D / CCS811 / Logging / NFC / WiFi / BLE) with OK/FAIL.
- **Live screen** — 3 aligned columns: label (x0) · value (x40) · unit (x80), warn/alert flag
  at x112. Rows: eCO2, TVOC, Temp, Hum. `°` via CP437 (0xF8).
- **Charts** — all **4 metrics on a single stats page** (stacked bands, 60 one-minute bars each).
- **Networks screen** — header `Networks:<total>` (+ animated `...` while scanning), then the
  strongest `NET_SHOW` (6) APs, **one line each**: `<name> 2.4G dev:n/a ~<dist>m` (always the
  2.4 GHz band on the C3; name auto-shortened to fit the remaining width).
- **Devices screen** — header `Devices:<total>` (nearby BLE) with an animated `...`, then up to
  `BLE_SHOW` (5) rows of device name (or MAC) + `~<dist>m`.
- **Button:** a single button (**D6**) cycles **live → stats → networks → devices → live**. (Button 2 / D7 wired but unused.)
- **Alarm** — on any sev-2, a full-screen WARNING **pulses** (`ALARM_SHOW_MS` on /
  `ALARM_GAP_MS` gap) and the buzzer plays **3 short beeps once** per warning (latched;
  re-fires only after readings return to normal and cross again). **Pressing D6 dismisses**
  an active alarm — silences the buzzer and returns to live readings; the alarm re-arms only
  after readings return to normal.

## 7. Logging & NFC
- **`logRow()`** writes CSV to SD (`SD_LOG_PATH`), else internal flash via LittleFS
  (`FLASH_LOG_PATH`). Flash cap (`FLASH_LOG_MAX`) currently **resets** the file when hit
  (coarser than the RP2040 ring-trim — simplify TODO).
- **Header (`LOG_HEADER`):** `uptime_s, eco2_ppm, tvoc_ppb, temp_c,
  humidity_pct, alarm`. Changing columns → **delete the old `kutsu_log.csv`** so a fresh
  header is written.
- **WiFi/BLE snapshot logs (SD only):** `SD_NET_LOG` (`/kutsu_net.csv`,
  `uptime_s,ssid,bssid,band,freq_mhz,channel,rssi,dist_m`) and `SD_DEV_LOG` (`/kutsu_dev.csv`,
  `uptime_s,mac,name,rssi,dist_m`). A full snapshot of all seen APs / all non-stale BLE devices
  is appended every `LOG_SCAN_INTERVAL_MS` (~1 min). **If no SD card is present these are not
  logged** (no flash fallback). SSID/name are quoted.
- **NFC:** `nfcProvision()` writes a fixed plain-text NDEF record (name/DOB/blood from
  `config.h`) to the NT3H2111 once at boot. Block map flagged to verify on hardware.

## 8. KNOWN ISSUES / OPEN ITEMS
1. **MQ-2 combustible-gas sensor removed.** It was dropped from this build: the ppm
   conversion was unreliable and, being an analog input, the pin has no presence check —
   a floating A0 (sensor unplugged) still read plausible noise and reported OK. All gas
   code (metric, chart, threshold, alarm, log column, boot line) is gone. If a gas sensor
   returns later, prefer a digital/I²C part so presence can actually be probed.
2. **SGP40 VOC-index sensor removed.** It duplicated the CCS811's air-quality reading
   (two "VOC-ish" numbers on the same screen) and confused end users. Air quality is now
   the CCS811 eCO2/TVOC only. All VOC code (metric, chart, threshold, warmup, log column,
   boot line) is gone.
3. **CCS811 run-in** — `eCO2 ≈ 400 ppm` / `TVOC ≈ 0–10 ppb` at rest is expected, not a bug;
   baseline needs ~20 min run-in / 48 h burn-in.
4. **No RTC** — log timestamps are uptime seconds. NTP planned with Wi-Fi.
5. **Flash-log trim** is a coarse reset (see §7).
6. **WiFi/BLE monitoring** — passive scanners with inherent limits: AP **client count is `n/a`**
   (not in beacons); **BLE is LE-only** (no Classic), unnamed advertisers show their MAC;
   **distances are rough** RSSI estimates (calibrate `*_RSSI_REF_1M` / `PATH_LOSS_N`). The C3 is
   **2.4 GHz only** — every AP shows the `2.4G` band and no 5 GHz APs appear. NimBLE code targets
   the **2.x** API. Both radios scan **continuously in the background** (for live data + SD logging),
   so power draw is higher than sensors-only; the C3 is **single-core**, so those tasks share a core
   with the 1 Hz loop — tune `WIFI_SCAN_INTERVAL_MS` / `LOG_SCAN_INTERVAL_MS` if responsiveness suffers.
7. **C3 strapping pins** — D8/D9 (GPIO8/GPIO9) are boot-strapping pins; a MicroSD on SCK=D8/MISO=D9
   can block normal boot. Remove the card while resetting, or relocate SCK/MISO (update `config.h`).

## 9. Build / flash / verify
See `README.md` for the board package + libraries. Then: compile in Arduino IDE → upload →
Serial Monitor @ **115200**. Boot prints an I²C scan (`found 0x3C/40/55/5A`), init results,
and the log target. Then: welcome → live readings; D6 cycles live → stats → networks →
devices → live (APs/BLE populate a second or two after opening each scan screen);
warm/humidify a sensor past a threshold → pulsing WARNING + 3 beeps, then press D6 to
dismiss; pull SD card → `kutsu_log.csv` populated.

## 10. Not yet built (roadmap)
The Wi-Fi/BLE **monitoring** screens (§6) are built. What remains is outbound
**connectivity**:

- **BLE → companion app** (Android / Expo)
- **Wi-Fi → dashboard**, plus NTP time

Both will build on the shared JSON data contract that's already implemented in the
Stationary firmware: see
[`DATA_CONTRACT.md`](../../../KUTSU%20Stationary/Firmware/KutsuSense/DATA_CONTRACT.md).
