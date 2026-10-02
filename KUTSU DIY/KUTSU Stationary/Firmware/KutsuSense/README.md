# KUTSU Sense — ESP32-C5-DevKitC-1 Firmware

[← Stationary firmware](../README.md) · [Hardware](../../HARDWARE.md) · [Internals](FIRMWARE.md) · [Wiring](WIRING.md) · [Data contract](DATA_CONTRACT.md)

Port of the Seeed **XIAO ESP32-C5** build ([`KutsuFirmwareXIAOESP32C5`](../../../KUTSU%20Handheld/Firmware/KutsuFirmwareXIAOESP32C5/README.md)) to the Espressif
**ESP32-C5-DevKitC-1**. Same environmental monitor (eCO₂/TVOC/temp/humidity, OLED,
charts, alarm, MicroSD logging, NFC health record) with these changes for this board:

1. **Pins are raw GPIO numbers** — the DevKitC has no `D0…D10` board macros.
2. **Three status LEDs** (green/yellow/red) mirror the alarm severity so a user
   with **no display** still gets a visual indicator.
3. **MQ-2 gas sensor** read on GPIO3.
4. **Outbound connectivity:** a BLE app link, a Wi-Fi master/slave mesh and a web dashboard,
   replacing the XIAO build's passive scan screens.

> [!NOTE]
> **Status: first port for this board — compile it in the Arduino IDE.** Logic is
> unchanged from the working XIAO build; only pins + the LED feature differ.

## 1. Board setup (Arduino IDE) — ESP32-C5-DevKitC-1 v1.2

1. **Install the LATEST esp32 core** — *File → Preferences →* add Espressif's boards URL,
   then *Tools → Board → Boards Manager → esp32 by Espressif →* **update to 3.3.x or newer**.
   This is the key step: older cores bundle **esptool 5.1.0**, whose C5 flash stub fails
   (`Changing baud rate to 921600` / `flash_id` → *the chip stopped responding*). The newer
   core ships a fixed esptool.
2. **Driver:** the board has two USB-C ports — **"USB"** (native USB-Serial-JTAG, no driver)
   and **"UART"** (bridge chip, needs its CP210x/CH34x driver). Easiest is the **"USB"** port.
3. **Board:** *Tools → Board → esp32 →* **ESP32C5 Dev Module**; pick the COM **Port** (prefer
   the native **"USB"** port).
4. **Tools settings that make it flash + fit:**
   - **USB CDC On Boot → Enabled** (Serial over USB-C)
   - **Upload Speed → 115200** (avoids the high-speed transfer that was corrupting)
   - **Flash Size → 4MB**
   - **Flash Mode → DIO**, **Flash Frequency → 40MHz** (gentler, more reliable)
   - **Partition Scheme →** BLE + WiFi + the web server make the binary much larger
     than the radios-off build, so use a **large-app scheme with an FS partition**:
     **Huge APP (3MB No OTA/1MB SPIFFS)** is the safe default here. `Default 4MB with spiffs`
     (1.28 MB app) is likely **too small now** — if the build reports the app overflowing its
     partition, switch to Huge APP. For over-the-air updates of deployed nodes, pick a
     **`… with OTA`** scheme instead (needs two app slots; confirm 4 MB fits, else use an 8 MB
     module). Avoid any scheme with **no** SPIFFS/FS partition or the flash-log fallback +
     the served dashboard assets can't mount. A `Preferences`/NVS partition is present in all
     of these (WiFi creds + room are stored there).
5. **Flashing:** click Upload. If it doesn't auto-enter the bootloader, hold **BOOT**, tap
   **RST**, release **BOOT**, then Upload. (One-time cleanup for a half-flashed board:
   enable *Erase All Flash Before Sketch Upload*, upload once, then turn it back off.)
6. The sketch folder must be named **`KutsuSense`** (matching `KutsuSense.ino`), with
   `config.h` beside it — as it is here.

### Upload fails: `Invalid head of packet ... Failed uploading` at 921600
The build is fine (it compiles small); this is a **serial-link** problem when esptool
switches to its high-speed second stage. In order of what fixes it:
1. **Use the board's other USB-C port.** The DevKitC-1 has two — silkscreen **"UART"**
   (USB-to-serial bridge, flaky at 921600) and **"USB"** (native USB-Serial-JTAG, baud
   is irrelevant). Move the cable to **"USB"**, reselect **Tools → Port** (COM number
   changes), and upload.
2. **`Tools → Upload Speed → 115200`** and confirm the log then says *"Changing baud
   rate to 115200"*. If it still says 921600 the setting didn't apply — reselect the
   board + port and set it again.
3. **Force download mode:** hold **BOOT**, tap **RST**, release **BOOT**, then Upload.
4. Use a **known-good data** USB-C cable plugged **directly** into the PC (no hub).

If the log *keeps* printing `Changing baud rate to 921600` no matter what Upload
Speed you pick, the C5 board profile is forcing 921600 and 921600 is failing on your
link. Two fixes:
- **Update the core:** Boards Manager → *esp32 by Espressif* → **Update** to the latest
  (C5 flashing is much more robust on 3.3.x+). Retry.
- **Bypass Arduino's baud** (guaranteed): Preferences → enable *verbose upload*, upload
  once, copy the full `esptool … write_flash …` line from the console (it has the exact
  binary paths + flash offsets for your build), then rerun it in a terminal with
  `--baud 921600` changed to **`--baud 115200`** (board in download mode). esptool then
  never jumps to 921600, so the corruption can't occur.

### Upload gets past the stub, then `A serial exception error occurred: Write timeout`
Port-access / USB-link issue (not baud, not code). In order:
1. **Close the Serial Monitor / Plotter** and any other terminal on the port, plus any
   stuck `esptool`/`python` in Task Manager — only one app can own the port.
2. **Disable USB power management:** Device Manager → the COM port → Power Management →
   uncheck *"Allow the computer to turn off this device…"*; and Power Options → USB
   selective suspend → Disabled.
3. Swap to the **other USB-C port** (now that baud is fixed, the previously-failing one
   may work) and reselect Tools → Port.
4. Known-good data cable into a **USB 2.0** port (no hub); download mode (BOOT+RST).

### Stub runs, then `flash_id` / `The chip stopped responding` (flash-attach fails)
The chip resets/drops right as esptool reads the SPI flash. Usually power or reset timing:
1. **Unplug the MQ-2** (its 5V heater ~150 mA) and other peripherals while flashing — the
   flash-write current spike can brown the board out off USB. Reconnect after programming.
2. **Download mode + retry 2–3×:** hold BOOT, tap RST, release BOOT, Upload (it's often
   intermittent on the C5).
3. **Gentler flash access:** Tools → Flash Mode → **DIO**, Flash Frequency → **40MHz**.
4. **Newer esptool + skip the stub** (the reliable fix if `flash_id` fails the same way
   every time — that's a bundled-esptool-5.1.0 C5 stub bug: the stub loads, then dies):
   - `pip install -U esptool`
   - Preferences → verbose upload → copy the full `esptool … write_flash …` line (exact
     .bin paths + offsets; note the **C5 bootloader is at 0x2000**, not 0x0).
   - Re-run it (the Arduino-bundled esptool is fine — `--no-stub` is what matters) with
     three edits: **`--baud 115200`**, add **`--no-stub`**, and **`--before no_reset`**;
     enter download mode first (hold BOOT, tap RST, release BOOT), then run. `--no-stub`
     uses the ROM loader (skips the broken 5.1.0 stub) and `--before no_reset` avoids the
     flaky RTS reset. This is the reliable fix — note the **latest esp32 core still bundles
     esptool 5.1.0**, so updating the core alone does NOT fix it; the Arduino Upload button
     always uses the bundled esptool with the stub, so it can't flash this board as-is.

## 2. Libraries
- Adafruit SSD1306
- Adafruit GFX Library
- Adafruit HTU21DF
- Adafruit CCS811
- Adafruit BusIO *(dependency)*
- **ArduinoJson** (bblanchon) — v7 (parse BLE writes / build dashboard responses)

**The dashboard uses the core's built-in `WebServer` (synchronous), NOT
ESPAsyncWebServer/AsyncTCP** — the async stack asserts `tcp_alloc ... Required to lock
TCPIP core functionality!` on ESP-IDF 5.x (it calls raw lwIP without the core lock). The
sync `WebServer` is safe; live dashboard updates use 2 s polling instead of a WebSocket.

**BLE uses the core's BUNDLED `BLE` library (`BLEDevice.h`), NOT NimBLE-Arduino.** On the
ESP32-C5 the core BLE library is NimBLE-backed (`CONFIG_BT_NIMBLE_ENABLED=y`) and compiled
against the exact C5 IDF BLE libs, so it runs correctly. The external **NimBLE-Arduino must
NOT be installed/used for the C5** — it links (after a symbol patch) but crashes at startup
(Load access fault) because its bundled host conflicts with the C5 controller libs. If you
have NimBLE-Arduino installed, it's fine as long as nothing `#include`s it.

`Wire`, `SPI`, `SD`, `LittleFS`, `WiFi`, `ESPmDNS`, `HTTPClient`, `Preferences`, `BLE` are
built into the ESP32 core. **Connectivity is back on this build** (BLE app link + WiFi node mesh +
master dashboard) — see the new modules `identity`, `ble_service`, `mesh`, `web_dashboard`
and `DATA_CONTRACT.md` (v2). This is the *outbound* link, unrelated to the old passive-scan
screens that were removed.

## 3. Wiring

See **[`WIRING.md`](WIRING.md)** for the full connection table + diagram. Summary:

| Peripheral | Pin(s) |
|---|---|
| **Power** — sensors/OLED/NFC/CCS811/SD | 3V3 / GND |
| I²C (HTU21D, OLED, NFC, CCS811) | SDA=**GPIO6**, SCL=**GPIO8** (+ external 4.7k pull-ups → 3V3) |
| CCS811 extras | **WAK → GND**, ADD → GND |
| MicroSD (SPI) | SCK=**GPIO23**, MISO=**GPIO24**, MOSI=**GPIO9**, CS=**GPIO10** |
| Push button (screen) | **GPIO1** → GND (INPUT_PULLUP) |
| Push button 2 (reserved) | *unwired* (would be GPIO25 — strapping) |
| Buzzer (active, +) | **GPIO4** (− → GND) |
| **LED — Green** (all good) | **GPIO5** → 330Ω → LED → GND |
| **LED — Yellow** (warning `!`) | **GPIO26** → 330Ω → LED → GND *(strapping pin — see note)* |
| **LED — Red** (alert) | **GPIO0** → 330Ω → LED → GND |
| **MQ-2** gas/smoke | VCC → **5V** (⚠ not 3V3); AO → **10k/10k divider** → **GPIO3** (ADC1_CH2); GND common |

All grounds common. The DevKitC-1 exposes only 10 fully-safe GPIOs, so two signals sit
on harmless strapping pins: the **yellow LED (GPIO26)** — a weak output load that can't
disturb the boot strap — and the **MQ-2 analog input (GPIO3)**, the only free ADC pin,
whose strap merely sets the unused SDIO clock edge. GPIO16–22 (incl. GPIO19) are **not
broken out** on this board. **The MQ-2 is a 5 V part** — its heater takes 5 V and its
analog output swings to ~5 V, so it goes through a ÷2 divider into GPIO3. See
[`WIRING.md`](WIRING.md) for the full rationale.

## 4. Features
- **Status LEDs** — worst-severity wins, one state at a time:
  - **GOOD** → green solid  ·  **WARNING `!`** → green **+** yellow solid
  - **MANY WARNINGS** (>3 metrics flagged at once) → **yellow solid only, green off** (conditions suboptimal)
  - **ALERT** → red **blinking**  ·  **ERROR** (a sensor failed to init) → yellow **blinking**
  - Power-on self-test blinks G→Y→R.
- **Live readings:** eCO2 / TVOC / Temp / Hum **/ Gas** (MQ-2). Stats page charts the 4
  environmental metrics (60 one-minute bars each); gas is live-only.
- **MQ-2 gas/smoke (now read):** GPIO3 ADC, shown as a **raw relative level (0–4095)** with
  a presence check (`--` when unpowered/unplugged). Not calibrated ppm — tune `GAS_WARN` /
  `GAS_HARM` in `config.h` against a real smoke test.
- **Button (GPIO1)** cycles live → stats → live (dismisses an active alarm if one is showing).
- **Alarm:** any metric at alert → full-screen warning pulses 5 s on / 1 s gap + **3 beeps
  once**, red LED blinks. Press the button to dismiss.
- **Logging: JSON Lines** — one JSON object per sample to SD (`/kutsu_log.jsonl`), else
  internal flash. Built to be sent straight to a **Wi-Fi dashboard** or **BLE app**; schema
  in [`DATA_CONTRACT.md`](DATA_CONTRACT.md).
- Fixed NFC health record written to the NT3H2111 at boot.

> [!NOTE]
> **Removed on this build:** the Wi-Fi AP-scan (*Networks*) and BLE device-scan
> (*Devices*) screens, plus their `/kutsu_net.csv` and `/kutsu_dev.csv` logs. The radios
> are used for **outbound** connectivity instead (BLE app link + Wi-Fi mesh/dashboard;
> see [FIRMWARE.md §10](FIRMWARE.md#10-connectivity-ble--wifi-mesh--implemented)).

## 5. Known differences / verify on first build
- **CCS811 needs `WAK → GND`** or `begin()` fails.
- **I²C pull-ups required** — the C5 has none onboard; add 4.7k on SDA and SCL.
- **`SPIClass spiSD(FSPI)`** — if the C5 core doesn't define `FSPI`, switch to `SPI` or `HSPI`.
- **GPIO0 (red LED)** — confirm your board revision doesn't tie GPIO0 to an onboard BOOT button; if it does, swap the red LED to another free pin.
- **GPIO26 (yellow LED)** — a strapping pin, used as an output LED only (safe as a weak load). If the board won't boot with it wired, move it to GPIO15 (no-PSRAM modules only) or run a 2-LED build.
- **Flash-log trim** resets the file at the cap (SD is the primary path).

## 6. Bring-up order
1. Compile + upload; open Serial Monitor at **115200**.
2. LEDs blink G→Y→R at power-on (self-test).
3. Confirm no init-failed lines; welcome → boot log → live readings appear.
4. Serial prints the I²C scan and an `MQ-2 raw=… (present/absent)` line — confirm the gas
   sensor reads `present` (else check its 5 V + the divider into GPIO3).
5. Breathe on the HTU/CCS811 sensors; the Gas row should react to a lighter/smoke nearby.
   Watch the LEDs: all-good = green solid; a warning = green+yellow; an alert = red blinking.
6. Press the button to cycle live → stats → live.
7. Push a metric past its alert threshold → red LED blinks + full-screen warning + 3 beeps;
   press the button to dismiss. Unplug a sensor (e.g. CCS811) → yellow LED blinks (error).
8. Check `/kutsu_log.jsonl` on the SD card — one JSON object per line (see `DATA_CONTRACT.md`).
