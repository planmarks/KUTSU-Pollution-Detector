# KUTSU PICO

> **Where it all started.** The original KUTSU Sense prototype: a Pimoroni **Pico LiPo
> (RP2040)** running **CircuitPython**, wired to off-the-shelf breakout modules on a
> protoboard.

[← Back to project overview](../README.md)

<p align="center">
  <img src="assets/welcome_logo_preview.png" alt="KUTSU Sense welcome screen" width="384">
</p>

> [!NOTE]
> This is the **legacy prototype**. CircuitPython doesn't support the ESP32-C5, so later
> generations moved to Arduino/C++: see [KUTSU DIY](../KUTSU%20DIY/README.md) and the current
> [KUTSU 1.0](../KUTSU%201.0/README.md). The code here is kept for reference and for anyone who
> wants a Python-based starting point.

---

## 📚 Documentation

| Document | What's inside |
|---|---|
| **[HARDWARE.md](HARDWARE.md)** | Components, pinout, wiring, I²C/power notes |
| **[FIRMWARE.md](FIRMWARE.md)** | CircuitPython install, module-by-module tour, thresholds, logging, NFC, test scripts |
| [lib/README.md](lib/README.md) | Required CircuitPython libraries |
| [kutsu.md](kutsu.md) | Feature sheet for the prototype |

---

## At a glance

| | |
|---|---|
| **MCU** | Pimoroni Pico LiPo 16 MB (PIM560), RP2040 with a built-in LiPo charger |
| **Firmware** | CircuitPython 10.x |
| **Sensors** | **SGP40** (VOC index) · **CCS811** (eCO₂ + TVOC) · **HTU21D** (temp + RH) · **MQ-2** (gas/smoke, raw) |
| **Display** | 0.96" 128 × 64 SSD1306 OLED |
| **Feedback** | Full-screen alarm + active buzzer playing **SOS in Morse** |
| **Storage** | CSV to MicroSD, or an internal-flash ring buffer |
| **Extras** | NFC tag (NT3H2111) carrying an emergency health record |

---

## What the device does

1. **Power on.** The welcome logo shows for about 2 s.
2. **Live screen.** Six rows, refreshed every second: **VOC · CO2 · TVOC · GAS · TEMP · HUM**.
3. **Press the button** to toggle between the live screen and the **one-hour charts** (VOC,
   temperature and humidity; one bar per minute).
4. **Harmful reading.** A full-screen warning shows for 5 s, hides for 1 s, and repeats while
   the condition lasts. The buzzer plays **SOS once** per new warning.
5. **Logging.** CSV goes to the SD card. Without a card, rows go to flash and the oldest ones
   are trimmed when it fills.
6. **NFC.** Any phone can tap the tag to read the owner's name, date of birth and blood type.

### Thresholds (`config.THRESHOLDS`)

| Metric | Warning `!` | Harmful (full-screen alarm) |
|---|---|---|
| VOC index (SGP40) | ≥ 150 | **≥ 400** |
| Temperature | ≥ 50 °C | **≥ 70 °C** |
| Humidity | ≥ 55 %RH | **≥ 70 %RH** |

VOC alarms are suppressed for the first **60 s** while the SGP40 learns its baseline. CCS811 and
MQ-2 values are **displayed only** and don't trigger alarms.

---

## Quick start

1. Flash **CircuitPython 10.x** for the *Pimoroni Pico LiPo (16MB)*.
2. Copy `lib/` to `CIRCUITPY/lib`.
3. Copy `code.py`, `boot.py`, `config.py`, `kutsu/` and `assets/` to the `CIRCUITPY` drive.
4. Reset the board.

> [!TIP]
> `boot.py` makes the flash writable for logging, which makes the `CIRCUITPY` drive
> **read-only to your computer**. **Hold the button while powering on** to keep the drive
> editable.

---

## Project layout

```text
KUTSU PICO/
├── code.py               entry point + main loop
├── boot.py               remounts flash read-write for logging (hold button to skip)
├── config.py             ALL tunables: pins, timing, thresholds, NFC record
├── kutsu/
│   ├── sensors.py        HTU21D · SGP40 · CCS811 · MQ-2 (graceful degradation)
│   ├── display.py        welcome / live / charts / alarm screens (displayio)
│   ├── history.py        60 × 1-minute ring buffers for the charts
│   ├── alarms.py         threshold evaluation (pure logic, desktop-testable)
│   ├── logging_store.py  SD-then-flash CSV logging with chronological trim
│   ├── nfc.py            hand-rolled NT3H2111 NDEF writer
│   └── buzzer.py         non-blocking Morse SOS on an active buzzer
├── assets/               welcome logo (.bmp) + generator script + preview
├── lib/                  CircuitPython libraries (from the 10.x bundle)
├── i2c_scan.py           bring-up: I²C bus scanner
├── sd_test.py            bring-up: MicroSD test (sdcardio)
├── sd_test2.py           bring-up: MicroSD test (adafruit_sdcard)
├── sgp_test.py           bring-up: raw SGP40 test
├── pico-bom.txt          bill of materials with supplier links
└── prototype-goal.txt    original prototype specification
```
