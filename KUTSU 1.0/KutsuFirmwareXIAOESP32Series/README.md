# KUTSU Sense: XIAO ESP32 Series Firmware

**Firmware version 0.10** · [Firmware internals →](FIRMWARE.md) · [Data contract →](DATA_CONTRACT.md) · [BLE provisioning contract →](PROVISIONING.md)

> Arduino/C++ firmware for the **KUTSU 1.0 board** (Kutsu 1, rev 3). **One sketch** runs on
> every Seeed XIAO that fits the board's footprint: **ESP32-C3, ESP32-C5, ESP32-C6 and ESP32-S3**.

### What's new in 0.10

- **KUTSU Hub replaces the on-device dashboard.** Stationary units now find the
  [KUTSU Hub](../KutsuHub/README.md), a Raspberry Pi / PC app, on the LAN and stream their
  readings and log to it, backlog included after an outage (§5). The ESP32-hosted dashboard and
  the master election are removed.
- New serial command `hub <host[:port]|auto>`. BLE STATUS gains `"hub"`. The last screen is now
  **Hub**.

### What's new in 0.9

- **Configurable logging rate** (`sample_ms`, 1–60 s) set from the app/serial — sensors still
  sample at 1 Hz (needed by the SGP41 VOC/NOx algorithm); only logging is decimated.
- **SD-log read-back over BLE** — the companion app imports stored readings via the HISTORY
  characteristic (write `{"cmd":"size"}` / `{"o":<offset>}`, read chunks).
- **Handheld stays off Wi-Fi** — a handheld unit no longer joins a saved network; only
  Stationary mode connects.
- **Optional BLE pairing/bonding** — behind `BLE_BONDING` in `config.h` (default off; see the
  flag's notes, as the Bluedroid security API may not build on a NimBLE-backed C5/C6 core).
- Firmware version is reported in the BLE STATUS payload as `"fw"`.

What it does:

- **Detects its sensors at boot.** An I²C scan decides what to initialise. It shows, charts and
  logs only the sensors it finds:
  - on-board Sensirion stack: **SCD41** (true NDIR CO₂), **SGP41** (VOC + NOx index),
    **SHT40** (temp/RH)
  - original DIY breakouts: **CCS811** (eCO₂ + TVOC) and **HTU21D** (temp/RH)
  - optional **MQ-2** gas/smoke module on D0
- **Works with either display.** It drives the on-board **0.96" OLED panel** (DS1) or a plug-in
  **SSD1306 128×64 module** at 0x3C or 0x3D. With no display it runs headless.
- **Feeds the KUTSU Hub in stationary mode.** On Wi-Fi it finds the
  [KUTSU Hub](../KutsuHub/README.md) (a collector + dashboard on a Raspberry Pi or PC) and
  streams its data there. Without Wi-Fi or a hub it keeps working stand-alone and logs to
  MicroSD, or to internal flash when no card is fitted.

> [!NOTE]
> **Status:** compiles cleanly for all four boards on esp32 core 3.3.11. It has not been tested
> on hardware yet. Follow the bring-up order in §7 for the first build.

---

## 1. Board setup (Arduino IDE)

1. **File → Preferences → Additional Boards Manager URLs**: add Espressif's ESP32 package URL.
2. **Tools → Board → Boards Manager**: install **esp32 by Espressif**, version **3.3.x or newer**.
   The C5 needs 3.3+.
3. Select your module and set the two options below:

| Module | Board | Partition Scheme | USB CDC On Boot |
|---|---|---|---|
| XIAO ESP32-C3 | `XIAO_ESP32C3` | **Huge APP (3MB No OTA/1MB SPIFFS)** | Enabled *(default)* |
| XIAO ESP32-C5 | `XIAO_ESP32C5` | **Huge APP (3MB No OTA/1MB SPIFFS)** | Enabled *(default)* |
| XIAO ESP32-C6 | `XIAO_ESP32C6` | **Huge APP (3MB No OTA/1MB SPIFFS)** | Enabled *(default)* |
| XIAO ESP32-S3 | `XIAO_ESP32S3` | default (8 MB, 3 MB app) | Enabled *(default)* |

- **Partition scheme.** The 1.7 MB image overflows the default 1.2 MB app partition on the 4 MB
  modules. Huge APP still leaves about 1 MB of LittleFS for the flash-log fallback. Don't use
  "Minimal SPIFFS".
- **USB CDC On Boot must stay Enabled** (the default on all four). On every one of these modules
  the two buttons (D6/D7) are the UART0 TX/RX pins. Without USB CDC, `Serial` runs on the buttons, the logs disappear
  and the D6 button fights the TX line. The sketch **refuses to compile** if this is off.

CLI equivalents:

```bash
arduino-cli compile -b esp32:esp32:XIAO_ESP32C3:PartitionScheme=huge_app KutsuSense
```
```bash
arduino-cli compile -b esp32:esp32:XIAO_ESP32C5:PartitionScheme=huge_app,CDCOnBoot=cdc KutsuSense
```
```bash
arduino-cli compile -b esp32:esp32:XIAO_ESP32C6:PartitionScheme=huge_app,CDCOnBoot=cdc KutsuSense
```
```bash
arduino-cli compile -b esp32:esp32:XIAO_ESP32S3 KutsuSense
```

> [!TIP]
> On Windows, if a build fails with *"opening dependency file … No such file or directory"*,
> the build path is too long. Pass a short one, for example `--build-path C:\kb\c6`.

## 2. Libraries

| Library | Used for |
|---|---|
| Adafruit SSD1306 + Adafruit GFX Library (+ Adafruit BusIO) | OLED panel / module |
| **Sensirion I2C SCD4x** (v1.x) → `SensirionI2cScd4x.h` | SCD41 CO₂ |
| **Sensirion I2C SHT4x** (v1.x) → `SensirionI2cSht4x.h` | SHT40 temp/RH |
| **Sensirion I2C SGP41** (classic) → `SensirionI2CSgp41.h` | SGP41 raw VOC/NOx |
| **Sensirion Gas Index Algorithm** (+ Sensirion Core) | raw → VOC/NOx Index |
| **Adafruit CCS811 Library** | DIY CCS811 breakout |
| **Adafruit HTU21DF Library** | DIY HTU21D breakout |
| **ArduinoJson** 7.x | BLE / hub JSON |

`WiFi`, `WiFiUdp`, `HTTPClient`, `Preferences`, `BLE`, `Wire`, `SPI`, `SD` and
`LittleFS` are built into the ESP32 core. Every library is required even if your unit doesn't
fit that sensor, because detection happens at runtime.

> **NimBLE-Arduino is no longer needed.** BLE scanning now uses the core's own `BLE` library. In
> core 3.3 that library is NimBLE-backed on all four chips. The external NimBLE-Arduino brings a
> second NimBLE host that crashes on the C5.

> **Header-casing gotcha:** the SCD4x/SHT4x drivers v1.x use lowercase `I2c`
> (`SensirionI2cScd4x`). The SGP41 driver is still the classic generation with uppercase `I2C`
> (`SensirionI2CSgp41.h`). If one fails to compile, you have the other generation installed.

Built and verified with: esp32 3.3.11 · Adafruit SSD1306 2.5.17 · GFX 1.12.6 · CCS811 1.1.3 ·
HTU21DF 1.1.2 · Sensirion SCD4x 1.1.0 · SHT4x 1.1.2 · SGP41 1.0.0 · Gas Index 3.2.3 ·
ArduinoJson 7.3.0.

## 3. Wiring / pin map

The XIAO pads are identical across the four modules. The board variant resolves each `Dx` to
the right GPIO, so `config.h` only uses `Dx` names.

| Function | Pad | C3 | C5 | C6 | S3 | Kutsu 1 rev 3 net / part |
|---|---|---|---|---|---|---|
| MQ-2 analog (optional) | D0 | 2 | 1 | 0 | 1 | `MQ2AO` (R2/R1 10k/20k divider) |
| Warning LED | D1 | 3 | 0 | 1 | 2 | `D1` → R15 → LED (active-high) |
| Buzzer | D2 | 4 | 25 | 2 | 3 | `GPIO4` → BZ1 + |
| MicroSD CS | D3 | 5 | 7 | 21 | 4 | `SDCS` |
| I²C SDA | D4 | 6 | 23 | 22 | 5 | `SDA` (R13 + R4 pull-ups) |
| I²C SCL | D5 | 7 | 24 | 23 | 6 | `SCL` (R14 + R3 pull-ups) |
| Button 1 | D6 | 21 | 11 | 16 | 43 | `BTN1` (also UART0 TX) |
| Button 2 (reserved) | D7 | 20 | 12 | 17 | 44 | `BTN2` (also UART0 RX) |
| MicroSD SCK | D8 | 8 | 8 | 19 | 7 | `SDCLK` |
| MicroSD MISO | D9 | 9 | 9 | 20 | 8 | `SDSO` |
| MicroSD MOSI | D10 | 10 | 10 | 18 | 9 | `SDSI` |

**I²C devices** (all probed at boot; only found parts are used):

| Part | Address | Where |
|---|---|---|
| SSD1306 OLED, panel or module | `0x3C` (`0x3D` alt) | DS1 on board / plug-in module |
| SCD41 | `0x62` | U3 on board |
| SGP41 | `0x59` | U1 on board |
| SHT40 | `0x44` (`0x45` for -BD1B) | U2 on board |
| HTU21D | `0x40` | DIY breakout |
| CCS811 | `0x5A` (`0x5B` alt) | DIY breakout (**WAK → GND**) |

## 4. Features

- **Auto-detected sensors.** The boot screen lists every module found (OK/FAIL) and any unknown
  I²C address. The live screen shows **only the metrics this unit has**, in this order: CO₂,
  eCO₂, TVOC, VOC, NOx, Temp, Hum, Gas. Up to 5 rows use a 12 px pitch, and up to 8 rows
  compress to fit.
  - **Temp/RH source priority:** SHT40, then HTU21D, then the SCD41's own T/RH.
  - **Compensation:** that T/RH feeds the SGP41 (every sample) and the CCS811 (every minute).
  - **MQ-2:** shown once it reads above `GAS_PRESENT_MIN`. On battery (no 5 V rail) it stays
    hidden.
- **Button D6** cycles **Live → Stats → Networks → Devices → Hub**. During an alarm it
  dismisses the alarm instead.
- **Stats:** one row per metric, showing the current value and a 60 × one-minute sparkline.
- **Networks / Devices:** passive background Wi-Fi and BLE scans. The C5 also sees 5 GHz APs.
  An AP's client count shows as `n/a`, and distances are rough RSSI estimates.
- **Hub screen:** online, it shows the hub address (open `http://<hub>/`), the sync state, the
  unit's IP, the SSID and the RSSI. Offline, it shows "Handheld mode" or "Stand-alone" with the
  Wi-Fi state and retry countdown.
- **Alarm:** on any sev-2 reading, a full-screen WARNING **pulses** (5 s on / 1 s gap) and the
  buzzer plays **3 short beeps once**. The **D1 LED** is solid on an alert, slow-blinks on a
  warning and is off when clear. D6 dismisses the alarm; the LED keeps indicating.
- **Buzzer drive:** the board's BZ1 is a magnetic transducer, so it's driven with a
  **2.7 kHz square wave** (`BUZZER_TONE_HZ`). For a DIY **active** buzzer, set it to `0`.
- **Logging:** **JSON Lines** (`/kutsu_log.jsonl`), see [DATA_CONTRACT.md](DATA_CONTRACT.md).
  - MicroSD gets every sample.
  - Without a card it goes to LittleFS: 1 sample in 10 plus every alarm sample, rotated at
    384 KB with two files kept.
  - The Wi-Fi/BLE scan snapshots go to `/kutsu_net.csv` and `/kutsu_dev.csv` about once a
    minute, on SD only.
- **Time:** once online, NTP (or the hub's clock) sets real time and records gain a `ts`
  epoch. Before that, `ts` is `null` and `up` (uptime) orders the records.

## 5. Stationary mode: sending data to the KUTSU Hub

The dashboard runs on a separate always-on machine on the LAN, not on the unit. That machine is
the **[KUTSU Hub](../KutsuHub/README.md)**, a Raspberry Pi or old Windows PC. A stationary unit
finds the hub, sends it a live reading every 5 s, and streams its log, so the hub keeps the full
history. The unit itself serves no web page.

```text
boot ──▶ mode? ── handheld ──▶ stand-alone, never joins Wi-Fi (BLE/USB only)
            │ stationary
            ▼
        credentials? ── no ──▶ stand-alone (display · alarm · SD/flash log · passive scans)
            │ yes
            ▼
        join Wi-Fi ── fail ──▶ stand-alone, retry every WIFI_RETRY_MS (60 s)
            │ ok
            ▼
        NTP time · find the hub (pinned address, or UDP broadcast on 47800)
            │ found
            ▼
        every 5 s: POST /api/live (current reading)
                   POST /api/ingest?src=log (new log lines; backlog after an outage)
        hub silent 30 s ──▶ rediscover        link lost ──▶ stand-alone (retrying)
```

- **No data lost to outages.** The unit remembers how far into its log the hub has received.
  Readings logged while the hub or Wi-Fi was down are sent when it comes back, catching up at
  up to 64 kB per 5 s. Streaming starts at the point the unit first reaches a hub after boot.
- **Time.** NTP when the network has internet. Otherwise the unit takes the hub's clock from
  its discovery reply after 10 s.
- **The Hub screen** (last on the D6 cycle) shows:
  - the hub address to open in a browser
  - "Data in sync" or "Catching up N kB"
  - the unit's IP, the SSID and the RSSI

**Setting a unit up.** Three ways:

1. **The Kutsu Setup app** (easiest): [`../KutsuSetupApp/`](../KutsuSetupApp/). A local desktop
   web app that connects **over Bluetooth or USB**. It sets the mode, network and room in a form,
   and links to the hub once the unit has found it. See [`../PROVISIONING.md`](../PROVISIONING.md).
2. **Over USB serial** (115200 baud, newline-terminated). Needs no reflash; values are stored in
   NVS, where they override `config.h`:
   ```text
   mode stationary
   ssid MyNetwork
   pass secret
   connect
   room Workshop        (optional; groups units on the hub dashboard)
   hub 192.168.1.20:8080  (only if broadcasts are blocked; `hub auto` to undo)
   status               (mode, Wi-Fi, hub, backlog, sensors, current record)
   forget               (disable Wi-Fi)
   ```
3. **In `config.h`** (compile-time defaults): `WIFI_SSID`, `WIFI_PASSWORD`, `ROOM_LABEL`,
   `HUB_ADDRESS`.

**Over Bluetooth (what the app uses).** With `PROVISION_BLE` enabled (default), the unit
advertises as `Kutsu-XXXXXX` and exposes the **Kutsu Environmental Service** GATT server
(`ble_provisioning.cpp`):

- write Wi-Fi credentials to the WIFI characteristic
- read or subscribe to STATUS, which now includes `hub`
- receive LIVE sample notifications

UUIDs and payloads are in `PROVISIONING.md`.

**Hub tunables** (`config.h`): `HUB_UDP_PORT` (47800, must match the hub), `HUB_PUSH_MS`
(5000), `HUB_TIMEOUT_MS` (30000), `HUB_NTP_GRACE_MS` (10000), `HUB_BATCH_BYTES` ×
`HUB_MAX_BATCHES` (4 kB × 16 per push).

> [!NOTE]
> Until 0.9 the first stationary unit on the LAN elected itself master and served a dashboard
> from the ESP32. That on-device dashboard and the mDNS election have been **removed** and
> replaced by the hub. The hub keeps history across reboots, handles many units, and doesn't
> load the unit's single radio.

## 6. Notes per module

| Module | Notes |
|---|---|
| **C3** | 2.4 GHz only. D8/D9 (GPIO8/9) are strapping pins. On the Kutsu 1 board, the SD pull-ups R6/R8 hold them high, which is the normal-boot level. If a DIY build won't boot with a card inserted, remove the card while resetting. |
| **C5** | Dual-band. The sketch enables 2.4 + 5 GHz scanning/joining (`WIFI_BAND_MODE_AUTO`). D2 is GPIO25, a strapping pin, and the BZ1 coil pulls it low at reset. If a C5 refuses to boot normally on the board, this is the first thing to check. |
| **C6** | 2.4 GHz Wi-Fi 6. The RF switch (GPIO3 enable, GPIO14 antenna select) is set by the board variant. Set `RF_USE_EXTERNAL_ANT 1` for the enclosure's whip antenna. |
| **S3** | 2.4 GHz. Dual core. Uses its default 8 MB partition table. |

All four modules have **one radio** shared by Wi-Fi and BLE. The BLE scan runs at 50 % duty
(`BLE_SCAN_WINDOW_MS` / `BLE_SCAN_INTERVAL_MS`) so Wi-Fi keeps enough airtime. A ~100 % duty
caused watchdog resets on the C3. Online, the passive AP scan only runs once a minute, because
each scan briefly takes the radio off-channel.

## 7. Bring-up order (recommended)

1. Compile and upload, then open the Serial Monitor at **115200**.
2. Check the I²C scan:
   - KUTSU 1.0 board: `0x3C 0x44 0x59 0x62`
   - DIY board: `0x3C 0x40 0x5A`

   The boot screen should list each found module as OK.
3. Breathe on the unit. CO₂ (or eCO₂) and VOC should climb, and Temp/Hum should look right.
4. Press D6 to cycle Stats → Networks → Devices → Hub.
5. Exhale or warm the unit past a threshold. You should get a pulsing WARNING, 3 beeps and a
   solid LED. Press D6 to dismiss.
6. Start the [KUTSU Hub](../KutsuHub/README.md) on a PC or Pi on the same network. On the unit,
   type `mode stationary`, `ssid …`, `pass …`, `connect`. Within about 20 s a "Hub connected"
   banner shows the hub address, and the unit appears on the hub dashboard.
7. Stop the hub for a minute, then restart it. The Hub screen should show "Catching up", and the
   gap should fill in on the hub's charts.
8. Switch the access point off. The Hub screen should fall back to "Stand-alone" with a retry
   countdown, and logging should continue.
9. Check `/kutsu_log.jsonl` on the SD card.

## 8. Known issues / verify on first build

- **Not yet tested on hardware.** Every module needs a bring-up pass.
- **OLED panel reset.** The panel's `RES#` is tied straight to 3V3 on rev 3, so there's no
  hardware reset pulse. The firmware fully re-initialises the controller on every boot. If the
  panel stays blank after a cold power-up, a power cycle should fix it. A future revision should
  add an RC delay or a GPIO on `RES#`.
- **Buzzer drive current.** BZ1 (18 Ω coil) hangs directly off a GPIO with no transistor. The
  tone drive makes it audible, but the GPIO is the current limit, so it won't reach the rated
  SPL. A transistor driver is recommended for the next board revision.
- **SGP40 isn't supported.** It answers at the same address as the SGP41 but has a different
  command set. It will show as `SGP41 FAIL`.
- **Provisional thresholds.** The CO₂, VOC, NOx and MQ-2 limits in `config.h` are starting
  points.
- **Sensor run-in.**
  - SGP41 indices read `--` for about the first minute: 10 s of NOx conditioning, then the
    algorithm's warm-up.
  - SCD41 CO₂ stabilises over minutes and benefits from fresh-air FRC.
  - The CCS811 needs about 20 min of run-in and 48 h of burn-in.
