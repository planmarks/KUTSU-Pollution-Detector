<div align="center">

# KUTSU Sense

**Open environmental monitoring for the air you breathe.**

*CO₂ · VOC · NOx · combustible gas & smoke · temperature · humidity*

![KUTSU 1.0 enclosure render](KUTSU%201.0/Case/Render.png)

</div>

---

## What is KUTSU?

KUTSU is an environmental monitoring and awareness device. It watches indoor air for the
things you can't see or smell, shows the readings on a small OLED display, and **warns you
the moment conditions turn unhealthy** with an on-screen alarm, a buzzer and status LEDs.
Every reading is logged to a MicroSD card, and newer builds can stream data to a phone app or
serve a self-hosted web dashboard. There's no cloud and no subscription.

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
| Build a **stationary node** with a web dashboard | [KUTSU DIY → Stationary](KUTSU%20DIY/KUTSU%20Stationary/README.md) |
| Hack on the original **CircuitPython** prototype | [KUTSU PICO](KUTSU%20PICO/README.md) |
| Read the **JSON data format** (BLE / Wi-Fi / SD) | [DATA_CONTRACT.md](KUTSU%20DIY/KUTSU%20Stationary/Firmware/KutsuSense/DATA_CONTRACT.md) |

---

## Generations at a glance

| | **KUTSU PICO** | **KUTSU DIY — Handheld** | **KUTSU DIY — Stationary** | **KUTSU 1.0** |
|---|---|---|---|---|
| **Status** | Original prototype | Legacy, available | Legacy, available | ✅ **Current** |
| **MCU** | Pimoroni Pico LiPo (RP2040) | Seeed XIAO ESP32-C3 / C5 | Espressif ESP32-C5-DevKitC-1 v1.2 | Seeed XIAO ESP32-C6 |
| **Firmware** | CircuitPython 10.x | Arduino / C++ | Arduino / C++ | Arduino / C++ |
| **CO₂** | CCS811 (estimated eCO₂) | CCS811 (estimated eCO₂) | CCS811 (estimated eCO₂) | **SCD41 (true NDIR CO₂)** |
| **VOC** | SGP40 index · CCS811 TVOC | CCS811 TVOC | CCS811 TVOC | **SGP41 VOC index** |
| **NOx** | — | — | — | **SGP41 NOx index** |
| **Temp / RH** | HTU21D | HTU21D | HTU21D | **SHT40** |
| **Gas / smoke** | MQ-2 (raw) | — (5 V part, no budget) | MQ-2 (raw) | MQ-2 header (optional) |
| **Feedback** | OLED · buzzer (SOS) | OLED · buzzer | OLED · buzzer · 3 LEDs | OLED · buzzer · warning LED |
| **Logging** | CSV → SD / flash | CSV → SD / flash | **JSON Lines** → SD / flash | CSV → SD / flash |
| **Connectivity** | — | Passive Wi-Fi/BLE scanner | **BLE app link + Wi-Fi mesh dashboard** | Passive Wi-Fi/BLE scanner |
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
3. **KUTSU 1.0.** The two branches merged onto **one board** with the **XIAO ESP32-C6**, and
   the sensors moved from CCS811 + HTU21D to the **Sensirion SCD41 / SGP41 / SHT40** stack.
   CO₂ is now measured rather than estimated.

---

## Roadmap

- [x] Sensirion SCD41 / SGP41 / SHT40 driver migration (KUTSU 1.0)
- [x] BLE app link + Wi-Fi mesh dashboard + JSON data contract (Stationary DIY build)
- [ ] Port the JSON data contract and outbound connectivity to the KUTSU 1.0 firmware
- [ ] BLE companion app (React Native / Expo)
- [ ] Real-time clock / NTP time source
- [ ] On-site calibration of CO₂, VOC, NOx and MQ-2 thresholds

---

## ⚠️ Safety notice

KUTSU is an air-quality **awareness and monitoring** instrument. The MQ-2 reports a *relative*
combustible-gas and smoke level. **Do not rely on KUTSU as a certified smoke, carbon-monoxide
or life-safety alarm**, and never use it as the only safety device in a life-safety
application. For regulatory or safety-critical use, validate readings against reference
instruments on site.

---

<div align="center">

*KUTSU — "6th Sense" · By Papathought*

</div>
