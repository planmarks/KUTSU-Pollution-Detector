# KUTSU Sense — Firmware Internals (ESP32-C5-DevKitC-1)

[← Stationary firmware](../README.md) · [Setup](README.md) · [Wiring](WIRING.md) · [Data contract](DATA_CONTRACT.md)

Handoff reference for the DevKitC-1 firmware. For board-package + library install
and the full wiring diagram, see `README.md` and `WIRING.md`. For all tunables,
see `config.h`.

## 1. What this is
An environmental monitor on the **Espressif ESP32-C5-DevKitC-1** (Arduino framework).
It reads five quantities (eCO2, TVOC, temp, humidity, **MQ-2 gas**), shows them on a
128×64 I²C OLED, charts the last hour, raises a pulsing full-screen alarm + 3-beep
buzzer on harmful readings, **drives three status LEDs**, logs **JSON Lines** to MicroSD
(flash fallback), and writes a fixed NFC health record.

**Lineage:** this is a port of [`KutsuFirmwareXIAOESP32C5`](../../../KUTSU%20Handheld/Firmware/KutsuFirmwareXIAOESP32C5/README.md) (Seeed XIAO ESP32-C5), itself a
port of the original [RP2040 + CircuitPython build](../../../../KUTSU%20PICO/README.md). CircuitPython does *not* support
the C5, hence Arduino/C++. Differences from the XIAO build: raw-GPIO pin map, the
3 status LEDs, the MQ-2, and **the WiFi/BLE passive-scan screens have been replaced by
outbound connectivity** (BLE app link + Wi-Fi mesh/dashboard, see §10).

## 2. Hardware & pins (see `config.h` / `WIRING.md`)
| Function | ESP32-C5 GPIO | Notes |
|---|---|---|
| I²C SDA / SCL | GPIO6 / GPIO8 | shared bus; **needs external 4.7k pull-ups → 3V3** |
| OLED / HTU21D / CCS811 / NTAG | I²C | 0x3C / 0x40 / 0x5A / 0x55 |
| CCS811 WAK / ADD | → GND | **WAK→GND required** or CCS811 won't init |
| MicroSD (SPI) | SCK 23, MISO 24, MOSI 9, CS 10 | `SPIClass(FSPI)` @ 1 MHz |
| Buzzer | GPIO4 | active buzzer (on/off) |
| Button 1 | GPIO1 | INPUT_PULLUP, active-low |
| Button 2 (reserved) | GPIO25 | **not wired** — strapping pin, unused in firmware |
| **LED green / yellow / red** | GPIO5 / **GPIO26** / GPIO0 | each via ~330Ω to GND, active-HIGH |
| **MQ-2 gas** | GPIO3 (ADC1_CH2) | VCC=**5V**; AO via **10k/10k ÷2 divider**; strap = unused SDIO edge |

All signal pins are non-strapping and clear of flash (GPIO15-18,20-22), USB
(GPIO13/14) and UART0 (GPIO11/12) — **except the yellow LED on strapping GPIO26**.
The DevKitC-1 exposes only 10 fully-safe GPIOs (0,1,4,5,6,8,9,10,23,24) and this
build needs 11 signals, so one LED (a weak output load that can't disturb the boot
strap) takes GPIO26. GPIO16-22 (incl. GPIO19) are not broken out on this board.
Sensors/OLED on **3V3**; common ground.

## 3. Measurements
Live-screen order: **eCO2, TVOC, Temp, Hum, Gas**.
| Shown as | Sensor | Unit | Nature |
|---|---|---|---|
| `eCO2` | CCS811 | ppm | **estimated** CO₂ (needs run-in/burn-in) |
| `TVOC` | CCS811 | ppb | concentration estimate (needs run-in/burn-in) |
| `Temp` | HTU21D | °C | calibrated absolute |
| `Hum` | HTU21D | %RH | calibrated absolute |
| `Gas` | MQ-2 (GPIO3 ADC) | raw ADC 0–4095 | **relative** level, not calibrated ppm; `--` if absent |

The MQ-2 is read via `readGas()` (averages `GAS_SAMPLES` ADC samples on GPIO3). A reading
below `GAS_PRESENT_MIN` means the sensor is unpowered/unplugged (the ÷2 divider pulls the
pin to GND), so it's treated as absent. Gas is on the live screen + JSON log + severity, but
is **not** charted (the 4-metric stats page has no room for a 5th band).

## 4. Alarm thresholds (`config.h`) — sev 1 = warning `!`, sev 2 = alert
| Metric | Warning `!` | Alert |
|---|---|---|
| eCO₂ | ≥ 900 ppm | ≥ 2000 ppm |
| TVOC | ≥ 660 ppb | ≥ 3000 ppb |
| Temp | 50–60 °C | < 0 or > 60 °C |
| Humidity | 25–30 or 60–70 %RH | < 25 or > 70 %RH |
| Gas (MQ-2) | ≥ `GAS_WARN` (raw) | ≥ `GAS_HARM` (raw) | **calibrate** to your sensor |

## 5. Status LEDs
`setLedMode(Eval, fault)` picks a mode each tick; `updateLeds()` renders it **every loop
iteration** so the blink is smooth (blink phase = `millis()/LED_BLINK_MS`). One state at a time:
| Mode | LEDs | When |
|---|---|---|
| GOOD | green solid | overall severity 0 |
| WARN | green **+** yellow solid | severity 1, up to `WARN_MANY_COUNT` metrics flagged |
| WARN_MANY | **yellow solid only** (green off) | severity 1 **and** > `WARN_MANY_COUNT` (default 3) metrics flagged — "conditions suboptimal" |
| ALERT | **red blinking** | overall severity 2 (any alert) — with OLED alarm + buzzer |
| ERROR | **yellow blinking** | a required sensor failed to init (`!okHtu || !okCcs`) |

`countWarnings(Eval)` counts metrics with severity ≥ 1 (of the 5: eCO2/TVOC/temp/hum/gas).
Priority: **ALERT > ERROR > WARN(/WARN_MANY) > GOOD** — an alert only comes from a working
sensor, so active danger outranks a failed-sensor error. Power-on `ledsSelfTest()` blinks G→Y→R.

## 6. Firmware architecture
Single sketch `KutsuSense.ino` + `config.h`. Top-to-bottom map is in the `.ino` header block.

**Data flow, once per second (`loop()` 1 Hz tick):**
`readSensors()` → `historyAdd()` → `evaluate()` → **`updateLeds()`** →
alarm-pulse-or-`renderScreen()` → `logRow()`. `updateBuzzer()` and `handleButtons()`
run every loop iteration. The passive Wi-Fi/BLE scanners were removed; the radios are
driven by the connectivity tasks `bleTask` / `netTask` instead (§10).

**Key structures:** `Reading` (one tick's values + `*Ok` flags), `Eval` (per-metric
severities 0/1/2, `harmful`, alarm `msg`/`detail`).

**Key functions:** `readSensors`/`readGas`, `historyAdd`/`metricValue`, `evaluate`,
`overallSeverity`, `setLedMode`/`updateLeds`/`ledsSelfTest`, `showWelcome`/`showLive`/
`showCharts`/`showAlarm`, `buildRecordJson`/`logRow`, `nfcProvision`, `handleButtons`.

## 7. UI behavior
- **Boot:** LED self-test (G→Y→R) → welcome ("KUTSU / 6th Sense / By Papathought")
  → boot-status screen listing each module (OLED / LEDs / HTU21D / CCS811 / **MQ-2** /
  Logging / NFC) with OK/FAIL.
- **Live screen** — 5 rows (eCO2/TVOC/Temp/Hum/**Gas**) at 12px pitch: label · value ·
  unit, warn/alert flag at x112.
- **Charts** — the 4 environmental metrics on a single stats page (60 one-minute bars each).
- **Button (GPIO1):** cycles live → stats → live; dismisses an active alarm instead
  if one is showing.
- **Alarm** — on any sev-2: full-screen WARNING pulses (`ALARM_SHOW_MS` on /
  `ALARM_GAP_MS` gap), buzzer plays 3 short beeps once (latched), **red LED blinks**.
  Pressing the button dismisses it (silences buzzer, back to live); LEDs still reflect
  live severity until readings recover.

## 8. Logging & NFC
- **Format: JSON Lines** — `logRow()` → `buildRecordJson()` writes one JSON object per
  sample to SD (`SD_LOG_PATH` = `/kutsu_log.jsonl`), else internal flash via LittleFS.
  Flash cap (`FLASH_LOG_MAX`) resets the file when hit (no header to rewrite).
- **Why JSON:** the same string is what a Wi-Fi dashboard / BLE app will transmit — one
  data contract, documented in **`DATA_CONTRACT.md`** (fields: `v,dev,up,sev,alarm,fault,
  eco2,tvoc,temp,hum,gas,s_*`). Missing sensors log `null`.
- **NFC:** `nfcProvision()` writes a fixed plain-text NDEF record (name/DOB/blood from
  `config.h`) to the NT3H2111 once at boot. Block map flagged to verify on hardware.

## 9. KNOWN ISSUES / OPEN ITEMS
1. **MQ-2 is read** as a raw relative ADC level (GPIO3), not calibrated ppm — tune
   `GAS_WARN`/`GAS_HARM`. Presence check via `GAS_PRESENT_MIN` (absent → `--`/`null`).
2. **SGP40 removed** (duplicated CCS811). Air quality = CCS811 eCO2/TVOC only.
3. **CCS811 run-in** — `eCO2 ≈ 400 ppm` / `TVOC ≈ 0–10 ppb` at rest is expected;
   baseline needs ~20 min run-in / 48 h burn-in.
4. **No RTC.** Log timestamps are uptime seconds until a phone syncs the time over BLE (§10). NTP over Wi-Fi is planned.
5. **Flash-log trim** is a coarse reset.
6. **WiFi/BLE scan screens removed** — the *Networks* and *Devices* screens and their
   `/kutsu_net.csv` + `/kutsu_dev.csv` logs are gone. Re-add from
   [`KutsuFirmwareXIAOESP32C5`](../../../KUTSU%20Handheld/Firmware/KutsuFirmwareXIAOESP32C5/README.md) if that monitoring is ever wanted again.
7. **Partition scheme:** still set **Tools → Partition Scheme → Huge APP
   (3MB No OTA/1MB SPIFFS)** and **Flash Size → 4MB** so the app never overflows the
   default 1.31 MB partition (dropping WiFi/BLE shrank the binary, but keep the headroom).
8. **Board-specific to verify:** `FSPI` defined by the C5 core (else use `SPI`/`HSPI`);
   GPIO0 (red LED) not tied to an onboard BOOT button on your board revision;
   GPIO26 (yellow LED) is a strapping pin — safe as an LED output, but if boot
   fails with it wired, move it to GPIO15 (no-PSRAM modules) or drop to 2 LEDs;
   `USB CDC On Boot` enabled for Serial over USB-C.

## 10. Connectivity (BLE + WiFi mesh) — IMPLEMENTED

Outbound link added in modules `identity` / `ble_service` / `mesh` / `web_dashboard`,
brought up as FreeRTOS tasks (`bleTask`, `netTask`) alongside the sensing `loop()`. The
synchronous `WebServer` is serviced from `netTask`. Shared state (latest sample snapshot, role, room,
boot-epoch) is guarded by two mutexes in `KutsuSense.ino` (`g_stateMtx`, `g_logMtx`); the
connectivity modules only ever touch the core through `kutsu_link.h`.

**Identity (2.3/2.4):** `identity.cpp` derives a stable name from the factory eFuse MAC —
suffix = last 3 MAC bytes → `kutsuName()` = `"Kutsu-A1B2C3"`, hostname `kutsu-a1b2c3`. BLE
advertises this name (the MAC is the BLE address; also exposed via the DIS serial-number
characteristic). No stored state, survives reflash, globally unique.

**BLE app link (`ble_service.cpp`, core `BLEDevice.h` — NimBLE-backed on the C5; NOT the
external NimBLE-Arduino, which crashes on this chip):** one Kutsu Environmental Service —
`LIVE` notify (the `buildRecordJson()` record, 1/sample), `TIMEGEO` write (`{epoch,lat,lon}`),
`WIFI` write (`{ssid,password,room}`), `STATUS` read/notify, `HISTORY` read (log tail). Keeps
advertising after WiFi connects so the app can always reach it.

**Time + geo (2.2):** the phone writes `TIMEGEO`; `kutsuApplyTimeSync()` sets
`bootEpoch = epoch − uptime`, stamps every later record with real `ts`, stores `lat/lon`, and
appends a one-off `{"ev":"timesync",…}` anchor line so the whole session's earlier records
become resolvable to absolute time.

**WiFi + node mesh (`mesh.cpp`, req 4):** creds arrive over BLE, persist in NVS. On
association the node advertises mDNS `_kutsu._tcp` and browses for a master. **No master →
become MASTER** (starts `web_dashboard`); **master found → become SLAVE** (POSTs each record
to the master's `/api/ingest`). Election clash resolves by **lower MAC**; a slave that loses
its master re-elects. Every node keeps its own durable JSONL on SD regardless of role (2.5).

**Dashboard (`web_dashboard.cpp` + `index_html.h`):** the master serves a self-contained SPA
at `http://kutsu-<suffix>.local/` — building **overview** (one card per room, coloured by the
record's `sev`) + **per-node drill-down** — from a per-node registry in RAM. Uses the core's
**synchronous `WebServer`** (the async stack asserts on ESP-IDF 5.x core-locking); browsers
**poll `/api/nodes` every 2 s** for live updates instead of a WebSocket.

**Companion app:** React Native + Expo (dev-client) with `react-native-ble-plx`,
`expo-location`, `react-native-maps`, an SVG/victory chart lib, and `expo-sqlite` for on-device
record storage. The app is not in this repository (firmware only).

## 11. Data contract v2
`buildRecordJson()` now emits `id/mac/role/room` + `ts` (real epoch, `null` until synced) +
`lat/lon/loc_ts`, and the log can contain `timesync` event lines. Full schema in
**`DATA_CONTRACT.md`**; `SCHEMA_VERSION` bumped to **2**.
