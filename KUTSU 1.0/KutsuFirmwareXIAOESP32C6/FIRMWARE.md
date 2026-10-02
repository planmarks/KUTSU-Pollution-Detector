# KUTSU Sense — Firmware Internals (XIAO ESP32-C6)

[← KUTSU 1.0](../README.md) · [Hardware](../HARDWARE.md) · [Firmware setup](README.md)

Handoff reference for the **new-hardware** firmware. For board package + library
install and the wiring table, see `README.md`. For all tunables, see `config.h`.

## 1. What this is
An environmental monitor on the **Seeed XIAO ESP32-C6** with on-board Sensirion
sensors. It reads five quantities, shows them on a 128×64 I²C OLED, charts the last
hour, raises a pulsing full-screen alarm + 3-beep buzzer + warning LED on harmful
readings, and logs CSV to MicroSD (flash fallback). It also passively scans nearby
WiFi APs (2.4 GHz) and BLE devices on two dedicated screens.

**Two builds of the same product:**
- [`KutsuFirmwareXIAOESP32C3`](../../KUTSU%20DIY/KUTSU%20Handheld/Firmware/KutsuFirmwareXIAOESP32C3/README.md) is the **DIY branch**: solder-on HTU21D + CCS811 modules, with
  NFC. It's kept unchanged for makers using the older parts.
- `KutsuXIAOESP32C6/` — **this** pre-assembled branch: on-board SCD41/SHT40/SGP41,
  NFC removed, warning LED on D1.

## 2. Hardware & pins (see `config.h` / `README.md`; source = board netlist)
| Function | XIAO pin | Notes |
|---|---|---|
| I²C SDA / SCL | D4 / D5 | on-board pull-ups R10/R11 |
| SCD41 / SHT40 / SGP41 / OLED | I²C | 0x62 / 0x44 / 0x59 / 0x3C |
| MicroSD (SPI) | SCK D8, MISO D9, MOSI D10, CS D3 | `SPIClass(FSPI)` @ 1 MHz |
| Buzzer | D2 | active buzzer (on/off) |
| Button 1 / Button 2 | D6 / D7 | INPUT_PULLUP, active-low |
| Warning LED | D1 | active-high (D1 → R15 → LED → GND); on-board on rev 3 |
| RF switch / antenna | GPIO3 / GPIO14 | GPIO3 LOW = enable; GPIO14 LOW = on-board ant |

Raw C6 GPIOs: D0=0 D1=1 D2=2 D3=21 D4=22 D5=23 D6=16 D7=17 D8=19 D9=20 D10=18.
D8/D9 are **not** strapping pins on the C6 (they are on the C3), so SD is safe here.

## 3. Measurements
Live-screen order: **CO₂, VOC, NOx, Temp, Hum**.
| Shown as | Sensor | Unit | Nature |
|---|---|---|---|
| `CO2`  | SCD41 | ppm | **true NDIR** CO₂ (periodic mode, ~1 sample/5 s) |
| `VOC`  | SGP41 | index 1–500 | Sensirion VOC Index (100 = recent baseline) |
| `NOx`  | SGP41 | index 1–500 | Sensirion NOx Index (1 = baseline) |
| `Temp` | SHT40 | °C | calibrated absolute (authoritative) |
| `Hum`  | SHT40 | %RH | calibrated absolute (authoritative) |

SHT40's T/RH also compensates the SGP41. The SCD41's own T/RH is not displayed.

## 4. Alarm thresholds (`config.h`) — sev 1 = warning `!`, sev 2 = alert `!!`
| Metric | Warning `!` | Alert `!!` |
|---|---|---|
| CO₂ | ≥ 1000 ppm | ≥ 2000 ppm |
| VOC Index | ≥ 250 | ≥ 400 |
| NOx Index | ≥ 20 | ≥ 150 |
| Temp | 50–60 °C | < 0 or > 60 °C |
| Humidity | 25–30 or 60–70 %RH | < 25 or > 70 %RH |

Gas/CO₂ numbers are **provisional** — calibrate on site.

## 5. Firmware architecture
Single sketch `KutsuSense.ino` + `config.h`. Top-to-bottom map is in the `.ino` header.

**Data flow, once per second (`loop()` 1 Hz tick):**
`readSensors()` → `historyAdd()` → `evaluate()` → alarm-pulse-or-`renderScreen()` → `logRow()`.
`updateBuzzer()`, `updateLed()`, `handleButtons()` and `serviceScans()` run every loop iteration.

**Sensor order in `readSensors()`** (dependencies matter):
1. **SHT40** `measureHighPrecision()` → temp/RH → converted to RH/T ticks.
2. **SGP41** — first `SGP41_CONDITION_S` ticks call `executeConditioning()` (NOx warm-up);
   after that `measureRawSignals()` → raw VOC/NOx → `VOCGasIndexAlgorithm`/`NOxGasIndexAlgorithm`
   `.process()` → indices. The algorithms assume 1 Hz sampling (the loop tick).
3. **SCD41** — `getDataReadyFlag()` then `readMeasurement()`; CO₂ holds last-good between the
   ~5 s updates.

**Warning outputs:** buzzer = 3 beeps once per latched alarm (`startBeep`/`updateBuzzer`).
LED (`updateLed`) is severity-driven and independent of the buzzer dismiss: solid on sev 2,
~0.5 Hz blink on sev 1, off when clear.

**WiFi/BLE (background, non-blocking):** unchanged from the other builds — async WiFi scan
re-kicked every `WIFI_SCAN_INTERVAL_MS`, BLE via a NimBLE task callback into a mutex-guarded
aged table (`BLE_STALE_MS`). C6 WiFi is 2.4 GHz only. The **C6 RF switch** (GPIO3/GPIO14) is
configured in `setup()` before the radios start.

**Key structures:** `Reading` (one tick's values + `*Ok` validity flags), `Eval` (per-metric
severities, `harmful`, alarm `msg`/`detail`).

## 6. UI behavior
- **Boot:** welcome screen → boot-status screen listing OLED / SHT40 / SGP41 / SCD41 /
  Logging / WiFi / BLE with OK/FAIL.
- **Live screen** — 5 rows at 12 px pitch: label (x0) · value (x40) · unit (x84) · flag (x116).
- **Stats screen** — 5 compact rows: `LABEL value` + a 60-bin one-minute sparkline.
- **Networks / Devices** — as in the other builds (2.4 GHz APs; nearby BLE).
- **Alarm** — full-screen pulsing WARNING + 3 beeps + LED; D6 dismisses.

## 7. Logging
- **`logRow()`** → SD (`SD_LOG_PATH`) else LittleFS flash (`FLASH_LOG_PATH`, coarse reset at
  `FLASH_LOG_MAX`).
- **Header:** `uptime_s,co2_ppm,voc_index,nox_index,temp_c,humidity_pct,alarm`. Changing
  columns → delete the old `kutsu_log.csv` so a fresh header is written.
- **WiFi/BLE snapshot logs (SD only):** `/kutsu_net.csv`, `/kutsu_dev.csv` every
  `LOG_SCAN_INTERVAL_MS`.
- **No NFC** in this build.

## 8. KNOWN ISSUES / OPEN ITEMS
1. **C6 RF switch** — WiFi/BLE need GPIO3 LOW (switch enable) + GPIO14 (antenna select),
   set in `setup()`. Verify polarity against the Seeed XIAO ESP32-C6 wiki if the radio is weak.
2. **Sensor run-in** — SGP41 indices warm for ~10 s (conditioning) then settle over minutes;
   SCD41 CO₂ stabilizes over minutes and benefits from a fresh-air FRC calibration.
3. **Provisional thresholds** — CO₂/VOC/NOx limits in `config.h` are starting points.
4. **No RTC** — log timestamps are uptime seconds. NTP planned with Wi-Fi.
5. **Flash-log trim** is a coarse reset (see §7); SD is the primary path.
6. **OLED driver** — `DM-OLED096-636` assumed SSD1306; switch to SH110X if garbled.
7. **Netlist metadata is stale.** The schematic file name still says `c5c3`, the exported
   `.net` is from rev 2, and the XIAO symbol is `XIAO-ESP32-C5-SMD`. The fitted module is a C6
   (identical XIAO pads), and the firmware targets the C6.
8. **MQ-2 not read.** Rev 3 routes the MQ-2 divider to D0, but this firmware doesn't sample
   it yet.

## 9. Build / flash / verify
Board package + libraries per `README.md`; set **Partition Scheme = Huge APP**. Compile →
upload → Serial Monitor @ **115200**. Boot prints the I²C scan (`0x3C 0x44 0x59 0x62`), init
results, and the log target. Then: welcome → live readings; D6 cycles the screens; exhale/warm
past a threshold → WARNING + beeps + LED, D6 to dismiss; pull the SD card → `kutsu_log.csv`.

## 10. Not yet built (roadmap)
Outbound connectivity (BLE→app, Wi-Fi→cloud dashboard) + NTP time, shared with the DIY branch
via a common JSON data contract.
