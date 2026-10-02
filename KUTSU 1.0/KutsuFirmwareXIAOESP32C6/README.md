# KUTSU Sense — XIAO ESP32-C6 Firmware

[← KUTSU 1.0](../README.md) · [Hardware](../HARDWARE.md) · [Firmware internals →](FIRMWARE.md)

> Arduino/C++ firmware for the **pre-assembled KUTSU 1.0 board** (Kutsu 1, rev 3).

The **new-hardware / pre-assembled** branch of KUTSU Sense. Runs on the assembled
KUTSU PCB with a **Seeed XIAO ESP32-C6** and on-board Sensirion sensors:

- **SCD41** — true NDIR CO₂ (ppm) + temp/RH
- **SHT40** — precision temp/RH (authoritative for the displayed/logged values)
- **SGP41** — VOC Index + NOx Index (via Sensirion's Gas Index Algorithm)
- **DM-OLED096-636** — 128×64 I²C OLED

> **Older hardware + firmware are still available.** This folder targets the C6 +
> Sensirion sensors only. The previous hardware — **XIAO ESP32-C3**, **XIAO ESP32-C5**,
> and the **Espressif ESP32-C5-DevKitC-1 v1.2** — running the older sensor modules
> (**HTU21D** temp/RH + **CCS811** eCO₂/TVOC, with NFC, and the MQ-2 gas sensor on the
> DevKitC) is kept untouched in its own firmware variants:
> - [`KutsuFirmwareXIAOESP32C3`](../../KUTSU%20DIY/KUTSU%20Handheld/Firmware/KutsuFirmwareXIAOESP32C3/README.md): XIAO ESP32-C3 (2.4 GHz), DIY solder-on sensors.
> - [`KutsuFirmwareXIAOESP32C5`](../../KUTSU%20DIY/KUTSU%20Handheld/Firmware/KutsuFirmwareXIAOESP32C5/README.md): XIAO ESP32-C5 (2.4 + 5 GHz), DIY solder-on sensors.
> - [`KUTSU Stationary/Firmware/KutsuSense`](../../KUTSU%20DIY/KUTSU%20Stationary/Firmware/KutsuSense/README.md): ESP32-C5-DevKitC-1 v1.2 with the old sensors **+ MQ-2 + 3 status
>   LEDs**, plus the Wi-Fi dashboard/mesh and the JSON-Lines data contract ([`DATA_CONTRACT.md`](../../KUTSU%20DIY/KUTSU%20Stationary/Firmware/KutsuSense/DATA_CONTRACT.md)).

> [!NOTE]
> **Status: first build for this hardware.** Pin and address facts come from the board
> netlist and were re-checked against the rev 3 PCB (see [HARDWARE.md](../HARDWARE.md#pin-map)).
> Verify the C6 RF-switch antenna control and the OLED controller on first bring-up (§5).

## 1. Board setup (Arduino IDE)
1. **File → Preferences → Additional Boards Manager URLs**: add Espressif's ESP32
   package URL. The C6 needs a **recent** core (≥ 3.0.x). See the
   [XIAO ESP32-C6 getting-started guide](https://wiki.seeedstudio.com/xiao_esp32c6_getting_started/).
2. **Tools → Board → Boards Manager** → install **esp32 by Espressif** (3.x+).
3. Select **XIAO_ESP32C6** as the board and pick the USB serial port.
4. **Tools → Partition Scheme → "Huge APP (3MB No OTA/1MB SPIFFS)"** (or "No OTA
   (2MB APP/2MB SPIFFS)"). **Required:** WiFi + BLE + graphics + the Sensirion
   libraries overflow the default 1.2 MB app partition. Both schemes keep ≥1 MB of
   filesystem for the LittleFS flash-log fallback — don't use "Minimal SPIFFS"
   (190 KB FS < the 256 KB `FLASH_LOG_MAX`).
   CLI: `-b esp32:esp32:XIAO_ESP32C6:PartitionScheme=huge_app`.

## 2. Libraries
- Adafruit SSD1306
- Adafruit GFX Library
- Adafruit BusIO *(dependency)*
- **Sensirion I2C SCD4x** (v1.x, new-gen) → header `SensirionI2cScd4x.h` *(SCD41 CO₂)*
- **Sensirion I2C SHT4x** (v1.x, new-gen) → header `SensirionI2cSht4x.h` *(SHT40 temp/RH)*
- **Sensirion I2C SGP41** (classic) → header `SensirionI2CSgp41.h` *(SGP41 raw VOC/NOx)*
- **Sensirion Gas Index Algorithm**  *(raw → VOC/NOx Index)*
- **Sensirion Core** *(dependency of the Sensirion I2C libs)*
- **NimBLE-Arduino** *(BLE scanning; code targets the 2.x API)*

> **Header-casing gotcha:** the SCD4x/SHT4x drivers were rewritten in their v1.x
> release — headers/classes use lowercase `I2c` (`SensirionI2cScd4x`), and
> `SensirionI2cScd4x::begin()` takes the I²C address while data-ready is
> `getDataReadyStatus()`. The **SGP41** driver is still the classic generation with
> uppercase `I2C` (`SensirionI2CSgp41.h`). The sketch's includes reflect this exactly.
> If SCD4x/SHT4x ever fail to compile with the opposite casing, you've got the other
> generation installed — match the include to whichever header the library ships.

`WiFi`, `Wire`, `SPI`, `SD`, `LittleFS` are built into the ESP32 core.

## 3. Wiring
The XIAO Dx pads are identical to the C5/C3 layout; only the sensors and MCU differ.
On the rev 3 board the XIAO is **U4** (it was U3 on rev 2).

| Function | XIAO pad | Net / part |
|---|---|---|
| I²C SDA / SCL | **D4 / D5** | on-board pull-ups R10/R11 → 3V3 |
| SCD41 / SHT40 / SGP41 / OLED | I²C | 0x62 / 0x44 / 0x59 / 0x3C |
| MicroSD (SPI) | SCK **D8**, MISO **D9**, MOSI **D10**, CS **D3** | connector J1 |
| Buzzer (active, +) | **D2** | BZ1 |
| Button 1 / Button 2 | **D6 / D7** | SW1 / SW2 (INPUT_PULLUP) |
| **Warning LED** | **D1** | on-board on rev 3 (D1 → R15 → LED → GND, active-high); a hand-wired mod on older rev 2 boards |
| RF switch enable / antenna sel | GPIO3 / GPIO14 | module-internal (see §5) |

`D0` carries a legacy MQ-2 analog divider on the PCB (R8/R9) and is unused by this
firmware. All grounds common; sensors/OLED on 3V3.

## 4. Features
- **Five live metrics:** CO₂ (ppm), VOC Index, NOx Index, Temp (°C), Hum (%RH).
- **Button (D6) cycles 4 screens:** live → stats → networks → devices → live.
  (Dismisses an active alarm instead, if one is showing.)
- **Stats screen** — one compact row per metric: label + current value on the left,
  a 60-bin (one-minute) sparkline on the right.
- **Networks / Devices screens** — background WiFi (2.4 GHz) + BLE scans, same as the
  other builds. AP client count is `n/a`; distances are rough RSSI estimates.
- **Alarm + warning LED:** on any sev-2 reading a full-screen WARNING **pulses**
  (5 s on / 1 s gap) and the buzzer plays **3 short beeps once**. The **D1 LED**
  shows severity continuously — **solid** on an alert (sev 2), **slow blink** on a
  warning (sev 1), off when clear. **Pressing D6 dismisses** the alarm (silences the
  buzzer, back to live); the LED keeps indicating until readings return to normal.
- **Logging:** sensor CSV to SD (`/kutsu_log.csv`), else internal flash (LittleFS).
  WiFi/BLE snapshots to SD (`/kutsu_net.csv`, `/kutsu_dev.csv`) ~once/min, SD-only.

## 5. Known differences / verify on first build
- **C6 RF switch / antenna.** The C6 routes its radio through an RF switch:
  **GPIO3 must be driven LOW** to enable it and **GPIO14** selects the antenna
  (LOW = on-board, HIGH = external u.FL). The sketch sets these in `setup()`
  (`RF_SWITCH_EN_GPIO` / `RF_ANT_SEL_GPIO` / `RF_USE_EXTERNAL_ANT` in `config.h`).
  If WiFi/BLE find nothing, this is the first thing to check against the Seeed wiki.
- **WiFi is 2.4 GHz only** (C6 is Wi-Fi 6 on 2.4 GHz). Every AP shows the `2.4G`
  band; the sketch's 5 GHz channel-map branch is never exercised.
- **Sensor run-in.** SGP41 VOC/NOx indices read "warming" for the first
  `SGP41_CONDITION_S` (10 s) of NOx conditioning, then settle toward the 100/1
  baselines over minutes. SCD41 CO₂ needs a few minutes to stabilize and benefits
  from field calibration (FRC) in fresh air.
- **VOC/NOx and CO₂ thresholds are provisional** (`config.h`) — calibrate for your
  environment.
- **OLED controller.** `DM-OLED096-636` is treated as SSD1306 @ 0x3C. If the display
  is garbled, switch to an SH110X driver.
- **NimBLE-Arduino 2.x** API (`getScan()->getResults`, `getDevice(i)->getName()`).
  Upgrade if you have 1.x.
- **`SPIClass spiSD(FSPI)`** — the C6 core defines `FSPI`. Fine as-is.
- **No NFC / no MQ-2 / no CCS811 / no HTU21D** in this build (those belong to the
  DIY branch).

## 6. Bring-up order (recommended)
1. Compile + upload; open Serial Monitor at **115200**.
2. Confirm the I²C scan prints `0x3C 0x44 0x59 0x62` and the boot screen shows
   OLED/SHT40/SGP41/SCD41/Logging/WiFi/BLE = OK.
3. Breathe on the unit → CO₂ and VOC climb; confirm Temp/Hum look right.
4. Press D6 to cycle: stats → networks → devices → live.
5. Warm/humidify or exhale heavily past a threshold → pulsing WARNING + 3 beeps +
   LED solid; press D6 to dismiss (LED stays until it clears).
6. Check `/kutsu_log.csv` on the SD card (header: `uptime_s,co2_ppm,voc_index,
   nox_index,temp_c,humidity_pct,alarm`).
