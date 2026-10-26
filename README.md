<div align="center">

# KUTSU Sense

**Open environmental monitoring for the air you breathe.**

*CO₂ · VOC · NOx · combustible gas & smoke · temperature · humidity*

[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)
![MCU](https://img.shields.io/badge/MCU-ESP32--C6%20%7C%20C5%20%7C%20C3%20%7C%20S3%20%7C%20RP2040-informational)
![Sensors](https://img.shields.io/badge/sensors-SCD41%20%7C%20SGP41%20%7C%20SHT40-success)
![Wireless](https://img.shields.io/badge/wireless-Wi--Fi%206%20%7C%20BLE%205-orange)
![Firmware](https://img.shields.io/badge/firmware-Arduino%20C%2B%2B%20%7C%20CircuitPython-blueviolet)
![KiCad](https://img.shields.io/badge/PCB-KiCad%2010-314CB0)
![Open Hardware](https://img.shields.io/badge/hardware-open%20source-brightgreen)

<img src="KUTSU%201.0/Case/Render.png" alt="KUTSU 1.0 3D render: two enclosed units with external antennas, the front showing the OLED window and three buttons" width="540">

</div>

---

## What is KUTSU?

KUTSU is an environmental monitoring and awareness device. It watches indoor air for the
things you can't see or smell, shows the readings on a small OLED display, and **warns you
the moment conditions turn unhealthy** with an on-screen alarm, a buzzer and status LEDs.
Every reading is logged to a MicroSD card, and newer builds can stream data to a phone app or
feed a self-hosted dashboard (the KUTSU Hub) on a Raspberry Pi or spare PC. There's no cloud and no subscription.

KUTSU comes in three configurations:

| Configuration | For whom | Highlights |
|---|---|---|
| 🛠️ **Development platform** | Makers, researchers, civic & environmental projects | All buses and power broken out, fully open firmware |
| ✋ **Handheld** | On-site, walk-up spot checks | LiPo battery, compact, display-led UI, laser pointer |
| 🏠 **Stationary** | Long-term monitoring of rooms and large sites | 5 V powered, MQ-2 gas/smoke sensor, 5 V fan, Wi-Fi dashboard + multi-node mesh |

---

### Where to start

| I want to… | Go to |
|---|---|
| Understand or build the **current device** | [KUTSU 1.0](KUTSU%201.0/README.md) |
| Build a **handheld** from breakout modules | [KUTSU DIY → Handheld](KUTSU%20DIY/KUTSU%20Handheld/README.md) |
| Run the **site dashboard** for stationary units (Raspberry Pi / PC) | [KUTSU Hub](KUTSU%201.0/KutsuHub/README.md) |
| Build a **stationary node** from breakout modules (legacy) | [KUTSU DIY → Stationary](KUTSU%20DIY/KUTSU%20Stationary/README.md) |
| Hack on the original **CircuitPython** prototype | [KUTSU PICO](KUTSU%20PICO/README.md) |
| Read the **JSON data format** (Wi-Fi / SD) | [KUTSU 1.0 DATA_CONTRACT.md](KUTSU%201.0/KutsuFirmwareXIAOESP32Series/DATA_CONTRACT.md) (v4) · [Stationary](KUTSU%20DIY/KUTSU%20Stationary/Firmware/KutsuSense/DATA_CONTRACT.md) (v2) |

---

## Generations at a glance

| | **KUTSU PICO** | **KUTSU DIY — Handheld** | **KUTSU DIY — Stationary** | **KUTSU 1.0** |
|---|---|---|---|---|
| **Status** | Original prototype | Legacy, available | Legacy, available | ✅ **Current** |
| **MCU** | Pimoroni Pico LiPo (RP2040) | Seeed XIAO ESP32-C3 / C5 | Espressif ESP32-C5-DevKitC-1 v1.2 | Seeed XIAO ESP32-C6 / C5 / C3 / S3 |
| **Firmware** | CircuitPython 10.x | Arduino / C++ | Arduino / C++ | Arduino / C++ |
| **CO₂** | CCS811 (estimated eCO₂) | CCS811 (estimated eCO₂) | CCS811 (estimated eCO₂) | **SCD41 (true NDIR CO₂)**, CCS811 if fitted |
| **VOC** | SGP40 index · CCS811 TVOC | CCS811 TVOC | CCS811 TVOC | **SGP41 VOC index** |
| **NOx** | — | — | — | **SGP41 NOx index** |
| **Temp / RH** | HTU21D | HTU21D | HTU21D | **SHT40**, HTU21D if fitted |
| **Gas / smoke** | MQ-2 (raw) | — (5 V part, no budget) | MQ-2 (raw) | MQ-2 header (optional, raw) |
| **Feedback** | OLED · buzzer (SOS) | OLED · buzzer | OLED · buzzer · 3 LEDs | OLED · buzzer · warning LED |
| **Logging** | CSV → SD / flash | CSV → SD / flash | **JSON Lines** → SD / flash | **JSON Lines** → SD / flash |
| **Connectivity** | — | Passive Wi-Fi/BLE scanner | **BLE app link + Wi-Fi mesh dashboard** | Passive Wi-Fi/BLE scanner · BLE app link · **KUTSU Hub** (LAN dashboard on a Pi / PC) |
| **Extras** | NFC health tag | NFC health tag | NFC health tag | Laser marker · LiPo · power switch |

---

## How it works (all generations)

Every KUTSU build shares the same core design:

```text
            ┌──────────── once per second ────────────┐
 sensors ──▶ readSensors ─▶ history ─▶ evaluate ─▶ display / alarm ─▶ log row
            └──────────────────────────────────────────┘
                 buttons · buzzer · LEDs · radios run continuously in between
```

- **Sensing loop at 1 Hz.** Each sensor is read into a `Reading` struct. A missing or failed
  sensor is marked *invalid* and is never reported as a fake `0`.
- **Three severities.** Each metric is graded *good* / *warning* `!` / *alert* `!!`. Thresholds
  live in a single config file (`config.h` or `config.py`).
- **Alerts.** An alert brings up a pulsing full-screen warning (5 s on, 1 s gap) and a short
  buzzer pattern, plus LEDs where fitted. Press the screen button to dismiss it. It re-arms
  once the air recovers.
- **History.** The firmware keeps 60 one-minute averages per metric for the on-device
  one-hour charts.
- **Logging.** Data goes to the MicroSD card when one is present and falls back to internal
  flash otherwise. There is no RTC, so timestamps are uptime seconds until a phone or NTP
  supplies the time.

---

## Lineage

1. **KUTSU PICO.** The first prototype was a CircuitPython build on a Raspberry Pi Pico LiPo
   wired to Adafruit-style breakout modules.
2. **Move to ESP32.** CircuitPython does not support the ESP32-C5, so the firmware was
   rewritten in **Arduino/C++**. It then split into a **Handheld** build (XIAO, battery) and a
   **Stationary** build (DevKitC, 5 V, mesh dashboard).
3. **KUTSU 1.0.** The two branches merged onto **one board** and **one firmware**. It runs on
   any footprint-compatible XIAO (ESP32-C6, C5, C3 or S3). The sensors moved from CCS811 +
   HTU21D to the **Sensirion SCD41 / SGP41 / SHT40** stack, so CO₂ is now measured rather than
   estimated. The firmware detects what's fitted, so the old breakouts still work. Stationary
   units feed the KUTSU Hub, a dashboard and collector on a Raspberry Pi or PC.

---

## Roadmap

- [x] Sensirion SCD41 / SGP41 / SHT40 driver migration (KUTSU 1.0)
- [x] BLE app link + Wi-Fi mesh dashboard + JSON data contract (Stationary DIY build)
- [x] Port the JSON data contract to the KUTSU 1.0 firmware (XIAO C3/C5/C6/S3)
- [x] KUTSU Hub: LAN collector + dashboard (Raspberry Pi / Windows) for stationary units
- [x] NTP time source when on Wi-Fi (KUTSU 1.0)
- [ ] BLE app link on KUTSU 1.0 + companion app (React Native / Expo)
- [ ] On-site calibration of CO₂, VOC, NOx and MQ-2 thresholds

---

## ⚠️ Safety notice

KUTSU is an air-quality **awareness and monitoring** instrument. The MQ-2 reports a *relative*
combustible-gas and smoke level. **Do not rely on KUTSU as a certified smoke, carbon-monoxide
or life-safety alarm**, and never use it as the only safety device in a life-safety
application. For regulatory or safety-critical use, validate readings against reference
instruments on site.

---

## License

KUTSU Sense is released under the **[Apache License 2.0](LICENSE)**. This covers the firmware,
documentation and hardware design files (KiCad sources, Gerbers, 3D models). Third-party
libraries bundled in `KUTSU PICO/lib/` keep their own licenses.

---

<div align="center">

*KUTSU — "6th Sense" · By Papathought*

</div>
