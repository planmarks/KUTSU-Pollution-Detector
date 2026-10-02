# KUTSU Sense — XIAO ESP32-C5 Firmware

[← Handheld firmware](../README.md) · [Hardware](../../HARDWARE.md) · [Firmware internals →](FIRMWARE.md)

Arduino/C++ port of the RP2040 CircuitPython build, rewritten because
**CircuitPython does not support the ESP32-C5** (Arduino / PlatformIO /
MicroPython / ESP-IDF only). Since the port, the MQ-2 gas and SGP40 VOC sensors
have been removed — air quality is now the CCS811 (eCO2/TVOC) only.

> [!NOTE]
> **Status: first port — compile it in the Arduino IDE.** The logic mirrors the
> proven RP2040 build, but this hasn't been compiled on the C5 toolchain here.
> Expect to resolve a couple of library/board-macro details on first build (noted below).

## 1. Board setup (Arduino IDE)
1. **File → Preferences → Additional Boards Manager URLs**, add Espressif's ESP32
   package (the version that includes ESP32-C5 — follow the
   [XIAO ESP32-C5 getting-started guide](https://wiki.seeedstudio.com/xiao_esp32c5_getting_started/)).
2. **Tools → Board → Boards Manager** → install **esp32 by Espressif** (a version with C5 support).
3. Select **XIAO_ESP32C5** as the board and pick the USB serial port.

## 2. Libraries
- Adafruit SSD1306
- Adafruit GFX Library
- Adafruit HTU21DF
- Adafruit CCS811
- Adafruit BusIO *(dependency)*
- **NimBLE-Arduino** *(BLE scanning; code targets the 2.x API — see §5)*

`WiFi`, `Wire`, `SPI`, `SD`, `LittleFS` are built into the ESP32 core — nothing to install.

## 3. Wiring (XIAO ESP32-C5)

| Peripheral | Pin(s) | XIAO pad |
|---|---|---|
| **Power** — sensors/OLED/NFC/CCS811 | VIN → **3V3**, GND → GND | 3V3 / GND |
| I²C (HTU21D, OLED, NFC, CCS811) | SDA=**D4**, SCL=**D5** | shared bus |
| CCS811 extras | **WAK → GND**, ADD → GND | required for I²C |
| MicroSD (SPI) | SCK=**D8**, MISO=**D9**, MOSI=**D10**, CS=**D3** | + card GND/3V |
| Push button (screen) | one leg **D6**, other **GND** | INPUT_PULLUP |
| Push button 2 (reserved) | one leg **D7**, other **GND** | wired, no action yet |
| Buzzer (active, +) | **D2** | − → GND |

All grounds common.

## 4. Features
- **Button (D6) cycles 4 screens:** live → stats → networks → devices → live. (Dismisses an active alarm instead, if one is showing.)
- Live readings (eCO2/TVOC/TEMP/HUM); stats = all 4 metrics charted on one page (60 one-minute bars each).
- **Networks screen** — background scan of nearby WiFi APs (2.4 & 5 GHz), **re-scanned every 5 s**. One line per AP (strongest first): `<name> <2G/5G> dev:n/a <~dist m>` (name auto-shortened to fit; `dev` is always `n/a` — see below). Shows an animated `...` while a scan is running.
- **Devices screen** — continuous background scan of nearby **BLE** advertisers. Header shows the total nearby count with an animated `...`; each row is a device name (or MAC if unnamed) and an RSSI-based distance estimate.
- Alarm: warn `!` and full-screen warning (eCO2 900/2000 ppm, TVOC 660/3000 ppb, temp warn 50 / alert 60 °C, humidity warn 60 / alert 70 %RH). Warning **pulses** 5 s on / 1 s gap and fires **3 short beeps once** per warning. **Pressing the button dismisses the alarm** (silences the buzzer, back to live) until readings return to normal and cross again.
- Logging: sensor CSV to SD (`/kutsu_log.csv`), else internal flash (LittleFS). **WiFi/BLE snapshots** logged to SD as `/kutsu_net.csv` and `/kutsu_dev.csv` (~once/min); **SD-only — skipped if no card is present**.
- Fixed NFC health record written to the NT3H2111 at boot.

## 5. Known differences / verify on first build
- **CCS811 needs `WAK → GND`** or `begin()` fails (same as the RP2040 build).
- **WiFi/BLE monitoring caveats (by design):**
  - **AP client count can't be read** by a passive scanner (it isn't in beacons) → shown as `n/a`.
  - **BLE = LE only** (the C5 has no Classic Bluetooth). Only devices actively advertising appear; phones/laptops usually won't unless pairing. Unnamed advertisers show their MAC.
  - **Distances are rough** RSSI path-loss estimates — calibrate `WIFI_RSSI_REF_1M` / `BLE_RSSI_REF_1M` / `PATH_LOSS_N` in `config.h` for your environment.
  - **5 GHz scan** relies on the C5 Arduino core scanning both bands — verify on hardware; if only 2.4 GHz appears, it's a core/scan-config limitation, not this sketch.
  - Scans run **continuously in the background** (WiFi async every `WIFI_SCAN_INTERVAL_MS`; BLE via a task callback into an aged table, `BLE_STALE_MS`) so data/logs stay fresh on any screen — both are non-blocking to the environmental loop, but the radios are always active (higher power draw). SD snapshots are written every `LOG_SCAN_INTERVAL_MS`.
- **NimBLE-Arduino version:** the BLE code uses the **2.x** API (`getScan()->getResults(ms, false)`, `getDevice(i)->getName()`). If you have 1.x installed, adjust those calls or upgrade.
- **`SPIClass spiSD(FSPI)`**: if the C5 core doesn't define `FSPI`, switch to the SPI bus id it exposes (e.g. the default `SPI`), or `HSPI`.
- **Board macros** (`D0`…`D10`, `SDA`, `SCL`): provided by the Seeed XIAO variant — if any are undefined, replace with the raw GPIO numbers from config.h comments.
- **Flash-log trim is simplified** (resets the file at the cap) vs the RP2040 ring-trim — SD is the primary path; refine later if needed.

## 6. Bring-up order (recommended)
1. Compile + upload; open Serial Monitor at **115200**.
2. Confirm no init-failed lines (OLED/HTU21D/CCS811/Logging/NFC/WiFi/BLE); welcome then live readings appear.
3. Test each sensor (breathe on HTU/CCS811).
4. Press the button to cycle: stats → networks (APs appear after a scan) → devices (BLE appears after a scan) → live.
5. Warm/humidify a sensor past a threshold → warning + 3 beeps; press the button again to dismiss.
6. Check `/kutsu_log.csv` on the SD card.
