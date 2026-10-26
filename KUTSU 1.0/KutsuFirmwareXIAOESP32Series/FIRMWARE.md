# KUTSU Sense: Firmware Internals (XIAO ESP32 Series)

[← KUTSU 1.0](../README.md) · [Hardware](../HARDWARE.md) · [Firmware setup](README.md) · [Data contract](DATA_CONTRACT.md)

Handoff reference for the KUTSU 1.0 firmware. For the board package, libraries and pin table,
see `README.md`. For every tunable, see `KutsuSense/config.h`.

## 1. What this is

One Arduino sketch for the **Kutsu 1 board** that runs on any footprint-compatible Seeed XIAO:
**ESP32-C3, C5, C6 or S3**. It merges three earlier firmwares:

| Came from | What it brought |
|---|---|
| KUTSU 1.0 C6 build (previous version of this folder) | Sensirion SCD41/SGP41/SHT40 drivers, warning LED, 5-metric UI |
| [DIY Handheld C3/C5](../../KUTSU%20DIY/KUTSU%20Handheld/Firmware/README.md) | CCS811 + HTU21D breakout support, C3 radio fixes (USB CDC, 50 % BLE duty) |
| [DIY Stationary](../../KUTSU%20DIY/KUTSU%20Stationary/Firmware/KutsuSense/README.md) | MQ-2 reading, JSON-Lines data contract, Wi-Fi mesh + self-hosted dashboard |

Now carried over from Stationary:

- **BLE GATT provisioning** — the "Kutsu Environmental Service" (`ble_provisioning.cpp`)
  lets the [Kutsu Setup app](../KutsuSetupApp/) set WiFi/room + read status over Bluetooth
  (as well as USB). Contract: [`../PROVISIONING.md`](PROVISIONING.md).

Not carried over:

- **NFC** health tag (DIY branches): there's no NTAG on this board.
- **BLE HISTORY characteristic** (`…0006`, Stationary log-tail): reserved but not served
  here yet (no log-tail accessor on this build).
- **3-LED status scheme** (Stationary): this board has one warning LED.

## 2. Files

| File | Role |
|---|---|
| `KutsuSense.ino` | Core: board selection, detection, sensors, UI, alarm, scans, logging, serial console, `kutsu_link.h` implementation |
| `config.h` | Every tunable: pins, I²C addresses, thresholds, Wi-Fi, **KUTSU Hub client**, **BLE provisioning**, logging |
| `kutsu_link.h` | The **only** seam between the core and the network tasks (snapshot, time, role, room, radio lock) |
| `ble_provisioning.h/.cpp` | BLE GATT server (WIFI/STATUS/LIVE/TIMEGEO/**SETTINGS**) layered on the scanner; `bleProv` notify task. Set up WiFi/room + settings from the app over BLE |
| `settings.h/.cpp` | NVS-backed user config: toggles (buzzer/LED/display/scan), **mode**, **warning thresholds** (4.1), **buzzer signal** (4.2), **LED behaviour** per severity (4.3), device **name** (4.4/4.5, via `identity`). Read by the loop/`mesh.cpp`/`evaluate()`; see `PROVISIONING.md` |
| `mesh.h/.cpp` | `netTask`: **mode gate** (handheld stays off Wi-Fi), Wi-Fi join/retry, NTP, **KUTSU Hub client** (UDP discovery, live push, log-tail streaming with backlog catch-up) |
| `identity.h/.cpp` | Device **name** (4.4/4.5): random **creature name** at first boot (e.g. `Otter-C3`), user-overridable; MAC stays the stable id, host `kutsu-xxxxxx` unchanged |

## 3. Multi-board support

Everything chip-specific is decided at compile time from `CONFIG_IDF_TARGET_*`. Pins are XIAO
`Dx` names, which the selected board's variant maps to GPIOs (table in `config.h` /
`README.md`).

| Concern | Handling |
|---|---|
| Board name in UI/log (`hw`) | `BOARD_NAME` / `BOARD_SHORT` per target. An unsupported target is a compile `#error` |
| D6/D7 = UART0 on every module | `#error` unless **USB CDC On Boot** is enabled (the default on all four XIAO boards) |
| C6 RF switch | The variant's `initVariant()` enables it. `setup()` re-asserts it and applies `RF_USE_EXTERNAL_ANT` |
| C5 5 GHz | `WiFi.setBandMode(WIFI_BAND_MODE_AUTO)` under `SOC_WIFI_SUPPORT_5G`. `chanToFreq()` labels 5G APs |
| Single shared radio (all) | BLE scan at 50 % duty. Online, AP scans drop to 1/min. A radio mutex keeps scans and Wi-Fi association apart |
| BLE stack | The core `BLE` library (NimBLE-backed on all four in core 3.3). The external NimBLE-Arduino is not used because it crashes on the C5 |
| SPI | `SPIClass(FSPI)`, defined on all four |

## 4. Sensor detection (boot)

`setup()` runs `scanI2C()` over 0x01–0x7E into `i2cFound[]`. Then:

| Address | Part | Init | Validity check |
|---|---|---|---|
| 0x3C / 0x3D | SSD1306 panel or module | `display.begin(SWITCHCAPVCC, addr, true, false)` | found on the scan (`okOled`). With none found the frame buffer is still allocated and flushes are skipped (headless) |
| 0x44 / 0x45 | SHT40 | `sht4x.begin` | `serialNumber()` OK |
| 0x40 | HTU21D | `htu.begin(&Wire)` | library reset + user-register read |
| 0x59 | SGP41 | `sgp41.begin` | `executeSelfTest()` OK, result bits clear |
| 0x62 | SCD41 | `stopPeriodicMeasurement()` (500 ms) → `startPeriodicMeasurement()` | start OK |
| 0x5A / 0x5B | CCS811 | `ccs.begin(addr, &Wire)` | HW-ID + app start |
| D0 (analog) | MQ-2 | `readGas()` | raw ≥ `GAS_PRESENT_MIN`. Also latched later at runtime |

Any other address appears on the boot screen as `0xNN unknown`. The boot screen scrolls, so
long lists stay readable.

`fitted[metric]` records which metrics have a working source. The UI, charts and JSON log
iterate over `fitted[]` only.

`g_fault` is set when a detected part fails to init, or when no environmental sensor exists.
It is logged as `fault`.

## 5. Measurements

One metric table, `MD[]` (key / label / unit / decimals / chart range / alarm text), drives the
live screen, stats, JSON and alarm text.

| Metric | Source | Unit | Notes |
|---|---|---|---|
| `co2` | SCD41 | ppm | periodic mode, about 1 sample per 5 s, holds last-good between |
| `eco2` / `tvoc` | CCS811 | ppm / ppb | estimates; T/RH compensation pushed once a minute |
| `voc` / `nox` | SGP41 + Gas Index Algorithm | index | 10 s NOx conditioning, then index 0 (learning) is shown as `--` |
| `temp` / `hum` | SHT40 › HTU21D › SCD41 | °C / %RH | first available source wins; also feeds SGP41/CCS811 compensation |
| `gas` | MQ-2 on D0 | raw ADC | relative level; 8-sample average |

The read order in `readSensors()` matters: T/RH → SCD41 (fallback T/RH) → SGP41 (compensated)
→ CCS811 (compensated) → MQ-2.

## 6. Alarm thresholds (`config.h`)

sev 1 = warning `!`, sev 2 = alert `!!`.

| Metric | Warning `!` | Alert `!!` |
|---|---|---|
| CO₂ (SCD41) | ≥ 1000 ppm | ≥ 2000 ppm |
| eCO₂ (CCS811) | ≥ 900 ppm | ≥ 2000 ppm |
| TVOC (CCS811) | ≥ 660 ppb | ≥ 3000 ppb |
| VOC Index | ≥ 250 | ≥ 400 |
| NOx Index | ≥ 20 | ≥ 150 |
| Temp | > 50 °C | < 0 or > 60 °C |
| Humidity | < 30 or > 60 %RH | < 25 or > 70 %RH |
| Gas (MQ-2 raw) | ≥ 1800 | ≥ 2600 |

When several metrics are at alert at once, the alarm screen names the first in
`ALARM_PRIORITY`: CO₂, eCO₂, VOC, NOx, TVOC, Gas, Temp, Hum.

## 7. Tasks and data flow

```text
loop() (Arduino task) ── every pass ──▶ updateBuzzer · updateLed · handleButtons
                                        · serviceScans · serviceConsole
                      ── every 1 s ────▶ bleSnapshot → readSensors → historyAdd → evaluate
                                        → publish g_snap → alarm-pulse | notice | renderScreen
                                        → logRow
netTask  (mesh.cpp)   ── join / retry / NTP / find hub / push live + log tail (every HUB_PUSH_MS)
BLE host task         ── ScanCB::onResult → bleUpsert (mutex-guarded table)
```

**Shared state** (`kutsu_link.h`), each piece guarded by a mutex:

- `g_stateMtx`: snapshot of the latest sample, room, hub address
- `g_logMtx`: every SD/flash write, including the scan CSVs and the NTP anchor line
- `g_radioMtx`: held by `netTask` for a whole association attempt. `serviceScans()` only kicks
  an async AP scan if it can take the lock without waiting, and `netTask` waits out a scan
  already in flight.

**Wi-Fi state machine** (`netState()`):

| State | Meaning | Device behaviour |
|---|---|---|
| `NET_NO_CREDS` | no SSID (config.h empty, or `forget`), or **handheld mode** | stand-alone |
| `NET_CONNECTING` | association in progress (≤ `WIFI_CONNECT_MS`) | stand-alone; AP scans paused |
| `NET_ONLINE` | joined | NTP, find the hub, push to it (role `slave` once the hub accepts data) |
| `NET_OFFLINE` | join failed or link lost | stand-alone; retry after `WIFI_RETRY_MS` (0 = never) |

Credentials load from NVS (`Preferences`, namespace `kutsu`) when present; otherwise they come
from `WIFI_SSID` / `WIFI_PASSWORD`. A serial `connect` / `forget` writes NVS and raises
`s_reprovision`. `netTask` then tears down any session and re-runs from `NET_NO_CREDS`.
`WiFi.setAutoReconnect(false)` means all retries are paced by `netTask`.

**KUTSU Hub client** (`hubTick()` in `mesh.cpp`; protocol in [`../KutsuHub/README.md`](../KutsuHub/README.md)):

- **Finding the hub.** A pinned address (`HUB_ADDRESS` or NVS `hub`) is resolved directly.
  Otherwise a `{"kutsu_discover":1,…}` UDP broadcast goes to port `HUB_UDP_PORT`, and the first
  `{"kutsu_hub":1,"port":…}` answer or beacon wins. Hub packets also carry `time`, which is
  used as the clock when NTP hasn't answered within `HUB_NTP_GRACE_MS`.
- **Live.** Every `HUB_PUSH_MS`, `kutsuCurrentRecordJson()` is POSTed to `/api/live`.
- **History.** The NDJSON log is read with `kutsuLogReadChunk()` from the offset `s_sentOff`.
  - Each POST to `/api/ingest?src=log` carries whole lines only, up to
    `HUB_BATCH_BYTES` × `HUB_MAX_BATCHES` per tick.
  - `s_sentOff` starts at the log's end the first time a hub is reached after boot, once the
    clock is set, so every streamed line has a real `ts`.
  - It survives hub and Wi-Fi outages, so the backlog is delivered later.
  - If the log shrinks (flash rotation), streaming restarts from the new file's start.
  - Without SD or flash, the live record is POSTed to `/api/ingest` instead, so the hub still
    stores it.
- **Losing the hub.** No accepted push for `HUB_TIMEOUT_MS` sets the role back to `solo`. In
  auto mode the unit rediscovers; with a pinned address it keeps retrying it.
- The unit runs **no web server and no mDNS**. The earlier ESP32-hosted dashboard
  (`web_dashboard.cpp`, `index_html.h`) and the master election were removed in 0.10.

## 8. UI behaviour

- **Boot:** welcome ("KUTSU / 6th Sense / By Papathought"), then a scrolling init log: board,
  display address, each detected sensor OK/FAIL, unknown addresses, MQ-2, logging target, BLE,
  Wi-Fi (`joining` / `off`), unit ID.
- **Live:** one row per fitted metric. Label at x0, value at x40, unit at x84, flag at x116.
  ≤ 5 rows use a 12 px pitch, 6–8 rows use 64/n px. "No sensors found" if empty.
- **Stats:** the same rows, with a numeric value and a 60-bin one-minute sparkline at x68.
- **Networks / Devices:** strongest 6 APs / 5 BLE devices, with RSSI distance estimates.
- **Hub:** online, it shows the hub `ip:port` (or "Finding hub…" / "Trying …"), "Data in sync"
  or "Catching up N kB", the unit's IP, SSID and RSSI. Offline, it shows "Handheld mode" or
  "Stand-alone", the SSID or "not set", and "Joining…" or the retry countdown. Both show the log
  target and unit suffix.
- **Notice:** a 4 s "Hub connected <ip:port>" banner each time the unit starts feeding a hub.
  A button press clears it, and alarms take precedence.
- **Alarm:** pulses 5 s / 1 s, beeps 3 times once (latched), and the LED is solid. D6
  acknowledges it. It re-arms when everything returns below alert.
- **Rotation:** `DISPLAY_ROTATION` (0 / 2) flips the image for panels or modules mounted the
  other way round.

## 9. Logging

- `logRow()` → `buildRecordJson()` → `appendLogLine()`. Schema in [DATA_CONTRACT.md](DATA_CONTRACT.md).
- **SD** (if a card is present at boot): every sample, plus `/kutsu_net.csv` and
  `/kutsu_dev.csv` scan snapshots every `LOG_SCAN_INTERVAL_MS`.
- **Flash** (LittleFS, no card): 1 in `FLASH_LOG_EVERY_N` samples plus every alarm sample.
  `enforceFlashCap()` rotates `kutsu_log.jsonl` → `kutsu_log.1.jsonl` at `FLASH_LOG_MAX`.
- **No card and no flash:** `Log: off` on the Hub screen, and the unit keeps running.
- The SD card is probed only at boot. Hot-inserting a card needs a reset.

## 10. Serial console (115200, USB CDC)

| Command | Effect |
|---|---|
| `ssid <name>` | stage an SSID (spaces allowed, rest of line) |
| `pass <password>` | stage a password (never echoed) |
| `connect` | save SSID + password to NVS and (re)join |
| `forget` | save an empty SSID: Wi-Fi off, handheld only |
| `room <label>` | set and save the room label (groups the unit on the hub dashboard) |
| `hub <host[:port]>` / `hub auto` | pin the KUTSU Hub address, or go back to UDP auto-discovery; `hub` alone prints the setting |
| `status` | board, ID, Wi-Fi state/IP/role, mode, hub + backlog, sensors, log target, current JSON record |

## 11. Known issues / open items

1. **Hardware bring-up pending** on all four modules. The firmware compiles cleanly for each
   (esp32 core 3.3.11).
2. **OLED `RES#` tied to 3V3** on rev 3: no reset pulse. If the panel stays blank after a cold
   power-up, power-cycle it. Next revision: an RC delay or a GPIO.
3. **BZ1 driven directly from a GPIO** (18 Ω magnetic transducer, no transistor). Square-wave
   drive at `BUZZER_TONE_HZ` makes it audible, at reduced SPL.
4. **C5:** D2 = GPIO25 is a strapping pin and is loaded by BZ1. Verify normal boot.
5. **Hub traffic is unauthenticated HTTP:** trusted LAN only (see the hub README).
6. **Provisional thresholds** for CO₂, VOC, NOx and MQ-2.
7. **No RTC:** `ts` is `null` until NTP, the hub, or the app/USB sets the clock. Handheld-only sessions with no app connection are uptime-stamped.
8. **Netlist metadata is stale.** The project is named `…c5c3`, and the exported `.net` is from
   rev 2. Re-export the netlist from the rev 3 schematic.

## 12. Build / verify

The CLI build commands are in `README.md` §1. Then:

1. Upload, then open the Serial Monitor at 115200.
2. Check the `[i2c] found 0x..` lines and the `[boot]` lines.
3. Start the KUTSU Hub on a PC/Pi on the same LAN.
4. Type `mode stationary`, `ssid` / `pass` / `connect`, then `status`.
5. Watch for `[hub] using …` and `[hub] connected: dashboard at http://…`.
6. Open that URL in a browser.
