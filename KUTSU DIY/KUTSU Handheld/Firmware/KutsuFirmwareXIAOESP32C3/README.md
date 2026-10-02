# KUTSU Sense — XIAO ESP32-C3 Firmware

[← Handheld firmware](../README.md) · [Hardware](../../HARDWARE.md) · [Firmware internals →](FIRMWARE.md)

Arduino/C++ build of KUTSU Sense, **retargeted from the XIAO ESP32-C5 to the
XIAO ESP32-C3**. The two boards share the XIAO form factor (same `D0`–`D10` pad
labels), so the wiring is identical; the substantive difference is that the C3
has a **2.4 GHz-only** radio (no 5 GHz). Descends from the original RP2040
CircuitPython build. The MQ-2 gas and SGP40 VOC sensors were removed earlier —
air quality is now the CCS811 (eCO2/TVOC) only.

> [!NOTE]
> **Status: retargeted for the C3 — compile it in the Arduino IDE.** The logic is
> the proven C5 build with the C3's 2.4 GHz-only radio and strapping-pin caveats
> folded in (noted below). Verify the SD-on-strapping-pins behavior on hardware.

## 1. Board setup (Arduino IDE)
1. **File → Preferences → Additional Boards Manager URLs**, add Espressif's ESP32
   package URL (the C3 is supported by the stable core — no special build needed).
   See the [XIAO ESP32-C3 getting-started guide](https://wiki.seeedstudio.com/XIAO_ESP32C3_Getting_Started/).
2. **Tools → Board → Boards Manager** → install **esp32 by Espressif** (any recent version).
3. Select **XIAO_ESP32C3** as the board and pick the USB serial port.
4. **Tools → Partition Scheme → "Huge APP (3MB No OTA/1MB SPIFFS)"** (or "No OTA (2MB
   APP/2MB SPIFFS)"). **Required:** with WiFi + BLE + graphics this sketch is ~1.31 MB,
   which overflows the C3's *default* 1.2 MB app partition (`text section exceeds available
   space`). Both schemes above keep a ≥1 MB filesystem for the LittleFS flash-log fallback —
   don't use "Minimal SPIFFS" (190 KB FS < the 256 KB `FLASH_LOG_MAX`).
   CLI equivalent: `-b esp32:esp32:XIAO_ESP32C3:PartitionScheme=huge_app`.

## 2. Libraries
- Adafruit SSD1306
- Adafruit GFX Library
- Adafruit HTU21DF
- Adafruit CCS811
- Adafruit BusIO *(dependency)*
- **NimBLE-Arduino** *(BLE scanning; code targets the 2.x API — see §5)*

`WiFi`, `Wire`, `SPI`, `SD`, `LittleFS` are built into the ESP32 core — nothing to install.

## 3. Wiring (XIAO ESP32-C3)

Identical to the C5 wiring — the XIAO pad labels are the same on both boards.

| Peripheral | Pin(s) | XIAO pad |
|---|---|---|
| **Power** — sensors/OLED/NFC/CCS811 | VIN → **3V3**, GND → GND | 3V3 / GND |
| I²C (HTU21D, OLED, NFC, CCS811) | SDA=**D4**, SCL=**D5** | shared bus |
| CCS811 extras | **WAK → GND**, ADD → GND | required for I²C |
| MicroSD (SPI) | SCK=**D8**, MISO=**D9**, MOSI=**D10**, CS=**D3** | + card GND/3V; **D8/D9 are strapping pins — see §5** |
| Push button (screen) | one leg **D6**, other **GND** | INPUT_PULLUP |
| Push button 2 (reserved) | one leg **D7**, other **GND** | wired, no action yet |
| Buzzer (active, +) | **D2** | − → GND |

All grounds common.

## 4. Features
- **Button (D6) cycles 4 screens:** live → stats → networks → devices → live. (Dismisses an active alarm instead, if one is showing.)
- Live readings (eCO2/TVOC/TEMP/HUM); stats = all 4 metrics charted on one page (60 one-minute bars each).
- **Networks screen** — background scan of nearby WiFi APs (**2.4 GHz only** — the C3 has no 5 GHz radio), **re-scanned every 5 s**. One line per AP (strongest first): `<name> 2.4G dev:n/a <~dist m>` (name auto-shortened to fit; `dev` is always `n/a` — see below). Shows an animated `...` while a scan is running.
- **Devices screen** — continuous background scan of nearby **BLE** advertisers. Header shows the total nearby count with an animated `...`; each row is a device name (or MAC if unnamed) and an RSSI-based distance estimate.
- Alarm: warn `!` and full-screen warning (eCO2 900/2000 ppm, TVOC 660/3000 ppb, temp warn 50 / alert 60 °C, humidity warn 60 / alert 70 %RH). Warning **pulses** 5 s on / 1 s gap and fires **3 short beeps once** per warning. **Pressing the button dismisses the alarm** (silences the buzzer, back to live) until readings return to normal and cross again.
- Logging: sensor CSV to SD (`/kutsu_log.csv`), else internal flash (LittleFS). **WiFi/BLE snapshots** logged to SD as `/kutsu_net.csv` and `/kutsu_dev.csv` (~once/min); **SD-only — skipped if no card is present**.
- Fixed NFC health record written to the NT3H2111 at boot.

## 5. Known differences / verify on first build
- **CCS811 needs `WAK → GND`** or `begin()` fails (same as the RP2040 build).
- **WiFi/BLE monitoring caveats (by design):**
  - **AP client count can't be read** by a passive scanner (it isn't in beacons) → shown as `n/a`.
  - **BLE = LE only** (the C3 has no Classic Bluetooth). Only devices actively advertising appear; phones/laptops usually won't unless pairing. Unnamed advertisers show their MAC.
  - **Distances are rough** RSSI path-loss estimates — calibrate `WIFI_RSSI_REF_1M` / `BLE_RSSI_REF_1M` / `PATH_LOSS_N` in `config.h` for your environment.
  - **2.4 GHz only** — the C3 has no 5 GHz radio, so every AP is reported on the 2.4 GHz band (the sketch's 5 GHz channel-map branch is simply never exercised).
  - Scans run **continuously in the background** (WiFi async every `WIFI_SCAN_INTERVAL_MS`; BLE via a task callback into an aged table, `BLE_STALE_MS`) so data/logs stay fresh on any screen — both are non-blocking to the environmental loop, but the radios are always active (higher power draw). SD snapshots are written every `LOG_SCAN_INTERVAL_MS`. Note the C3 is single-core, so the always-on BLE task and WiFi scans share one core with the loop — keep an eye on responsiveness.
- **NimBLE-Arduino version:** the BLE code uses the **2.x** API (`getScan()->getResults(ms, false)`, `getDevice(i)->getName()`). If you have 1.x installed, adjust those calls or upgrade.
- **C3 strapping pins (D8/D9):** on the ESP32-C3, **D8 = GPIO8** and **D9 = GPIO9** are boot-strapping pins (GPIO9 is also the BOOT button). A MicroSD wired to SCK=D8 / MISO=D9 can hold those lines at levels that block normal boot. If the board hangs in download mode or won't run with a card inserted, reset with the card removed, or move SCK/MISO to other free pads and update `config.h`.
- **`SPIClass spiSD(FSPI)`**: the C3 core defines `FSPI` (maps to SPI2) — fine as-is. If a future core drops it, switch to the default `SPI`.
- **Board macros** (`D0`…`D10`, `SDA`, `SCL`): provided by the Seeed XIAO ESP32-C3 variant — if any are undefined, replace with the raw GPIO numbers (D2=GPIO4, D3=GPIO5, D4=GPIO6, D5=GPIO7, D6=GPIO21, D7=GPIO20, D8=GPIO8, D9=GPIO9, D10=GPIO10).
- **Flash-log trim is simplified** (resets the file at the cap) vs the RP2040 ring-trim — SD is the primary path; refine later if needed.

## 6. Bring-up order (recommended)
1. Compile + upload; open Serial Monitor at **115200**.
2. Confirm no init-failed lines (OLED/HTU21D/CCS811/Logging/NFC/WiFi/BLE); welcome then live readings appear.
3. Test each sensor (breathe on HTU/CCS811).
4. Press the button to cycle: stats → networks (APs appear after a scan) → devices (BLE appears after a scan) → live.
5. Warm/humidify a sensor past a threshold → warning + 3 beeps; press the button again to dismiss.
6. Check `/kutsu_log.csv` on the SD card.
