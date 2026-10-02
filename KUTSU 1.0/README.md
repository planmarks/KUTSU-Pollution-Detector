# KUTSU 1.0

> **One board, every use case.** KUTSU 1.0 merges the earlier Handheld and Stationary
> prototypes into a single PCB: the **Kutsu 1 board (rev 3)**. It's built around the Seeed
> XIAO ESP32-C6 and a laboratory-calibrated Sensirion sensor stack.

[← Back to project overview](../README.md)

<p align="center">
  <img src="Case/Render.png" alt="KUTSU 1.0 enclosure" width="45%">
  <img src="3D/Kutsu-PCB-Render.png" alt="Kutsu 1 board render" width="45%">
</p>

---

## 📚 Documentation

| Document | What's inside |
|---|---|
| **[HARDWARE.md](HARDWARE.md)** | Board overview, bill of materials, pin map, power, design files, fabrication |
| **[Firmware README](KutsuFirmwareXIAOESP32C6/README.md)** | Arduino IDE setup, libraries, wiring, features, bring-up checklist |
| **[FIRMWARE.md](KutsuFirmwareXIAOESP32C6/FIRMWARE.md)** | Firmware internals: architecture, UI, alarms, logging, known issues |
| [kutsu-project.md](kutsu-project.md) | Short product description |

---

## What it measures

| Reading | Sensor | Type | Units |
|---|---|---|---|
| **CO₂** | Sensirion **SCD41** | Photoacoustic NDIR (true CO₂) | ppm |
| **VOC index** | Sensirion **SGP41** | MOx gas sensor + Gas Index Algorithm | 1–500 |
| **NOx index** | Sensirion **SGP41** | MOx gas sensor + Gas Index Algorithm | 1–500 |
| **Temperature** | Sensirion **SHT40** | Digital T/RH | °C |
| **Humidity** | Sensirion **SHT40** | Digital T/RH | %RH |
| **Combustible gas / smoke** | **MQ-2** *(optional, external module)* | Analog MOx | relative |

**Why the upgrade matters:** the earlier builds used a CCS811, which *estimates* CO₂ from VOCs.
The SCD41 **measures** CO₂ directly. The SGP41 adds separate **VOC and NOx** indices, which
give a much richer picture than a single "air quality" number.

---

## The three configurations

All three use the **same Kutsu 1 board**. What changes is which parts are fitted, how the
board is powered, and which firmware features are enabled.

| Configuration | Power | Distinctive parts | Networking |
|---|---|---|---|
| 🛠️ **Development platform** | USB-C 5 V | I²C, SD, MQ-2 and power broken out to pads | Open firmware, full access |
| ✋ **Handheld** | LiPo battery (USB-C charging) | Compact, display-led UI, laser marker, small fan | BLE companion app *(roadmap)* |
| 🏠 **Stationary** | 5 V mains / USB | Optional **MQ-2** module, **5 V fan** | Wi-Fi dashboard + mesh *(roadmap)* |

One design to manufacture, one firmware codebase and one data format: a handheld unit and a
wall-mounted site unit speak the same language.

---

## Quick start

1. Install the **esp32 by Espressif** core (≥ 3.0) and select **XIAO_ESP32C6**.
2. Set **Partition Scheme → Huge APP (3MB No OTA/1MB SPIFFS)**.
3. Install the libraries listed in the [firmware README](KutsuFirmwareXIAOESP32C6/README.md#2-libraries).
4. Open `KutsuFirmwareXIAOESP32C6/KutsuSense/KutsuSense.ino` and upload.
5. Open the Serial Monitor at **115200**. The I²C scan should list `0x3C 0x44 0x59 0x62`.

---

## On-device experience

- **Boot.** A splash screen ("KUTSU / 6th Sense / By Papathought") is followed by a self-test
  screen that shows OK/FAIL for each module.
- **Screens.** One button cycles through **Live → Stats (1-hour sparklines) → Wi-Fi networks →
  BLE devices**.
- **Alerts:**

  | Channel | Behaviour |
  |---|---|
  | 🖥️ Display | Full-screen WARNING that pulses (5 s on / 1 s gap) until you dismiss it |
  | 🔊 Buzzer | 3 short beeps once per alert; re-arms after the air recovers |
  | 💡 Warning LED | Solid on alert, slow blink on warning, off when clear |

- **Logging.** CSV goes to MicroSD (`/kutsu_log.csv`), with internal flash as the fallback.
- **Laser marker.** A hardware switch lights a laser so you can point at the spot you're sampling.

### Default thresholds

| Metric | Warning `!` | Alert `!!` |
|---|---|---|
| CO₂ | ≥ 1000 ppm | ≥ 2000 ppm |
| VOC index | ≥ 250 | ≥ 400 |
| NOx index | ≥ 20 | ≥ 150 |
| Temperature | 50–60 °C | < 0 °C or > 60 °C |
| Humidity | 25–30 % or 60–70 %RH | < 25 % or > 70 %RH |

> [!NOTE]
> The CO₂, VOC and NOx limits are **provisional**. Calibrate them on site. You can change any
> of them in `config.h`.

---

## Directory contents

| Path | Contents |
|---|---|
| `kutsu-10-xiao-esp32c5c3.kicad_pro` / `.kicad_sch` / `.kicad_pcb` | KiCad project: **Kutsu 1, rev 3** |
| `kutsu-10-xiao-esp32c5c6.net` | Exported netlist *(older, rev 2; see [HARDWARE.md](HARDWARE.md#design-files))* |
| `KUTSUV1 Gerber/` + `KUTSUV1 Gerber.zip` | Fabrication gerbers + drill files |
| `3D/` | PCB render, board models (`.stl`, `.glb`) |
| `Case/` | Enclosure model (`Kutsu 1 Case.blend`) + render |
| `Kutsu Handheld 3D Model.blend` | Industrial-design model |
| `KutsuFirmwareXIAOESP32C6/` | Firmware for this board |
| `*.kicad_prl`, `fp-info-cache`, `.history/` | KiCad local/editor state, not part of the design |

---

## Other hardware variants

The XIAO footprint is pin-identical across C3, C5 and C6, so this PCB also accepts the older
modules. The earlier boards and their firmware are still available:

| Variant | MCU | Sensors | Where |
|---|---|---|---|
| **KUTSU 1.0** *(this)* | XIAO ESP32-C6 | SCD41 · SGP41 · SHT40 | [`KutsuFirmwareXIAOESP32C6`](KutsuFirmwareXIAOESP32C6/) |
| DIY Handheld | XIAO ESP32-C3 / C5 | CCS811 · HTU21D · | [`KUTSU DIY/KUTSU Handheld`](../KUTSU%20DIY/KUTSU%20Handheld/README.md) |
| DIY Stationary | ESP32-C5-DevKitC-1 | CCS811 · HTU21D · MQ-2 · 3 LEDs | [`KUTSU DIY/KUTSU Stationary`](../KUTSU%20DIY/KUTSU%20Stationary/README.md) |

---

## Roadmap

- [x] Sensirion SCD41 / SGP41 / SHT40 driver migration
- [ ] Port the JSON data contract, BLE app link and Wi-Fi mesh dashboard from the
      [Stationary firmware](../KUTSU%20DIY/KUTSU%20Stationary/Firmware/KutsuSense/README.md)
- [ ] BLE companion app (React Native / Expo)
- [ ] Real-time clock: phone or NTP time sync
- [ ] SCD41 fresh-air forced-recalibration (FRC) workflow; on-site threshold tuning

---

## For industrial & site deployment

- **Calibrated measurements.** True CO₂ (NDIR), VOC and NOx indices, temperature and humidity
  come from Sensirion sensors, not estimated proxies.
- **Standalone today, fleet-ready.** Each node works on its own: display, LED, alarm and SD log.
  The multi-node dashboard runs on the DIY Stationary firmware and is being ported to this build.
- **Your data stays yours.** Logs go to a removable MicroSD card in an open format. There's no
  cloud, no account and no per-seat fee.
- **Resilient.** Every node logs locally whatever the network state, and a failed sensor is
  marked invalid rather than reported as zero.
- **Flexible power.** Run it from 5 V mains with an active fan for permanent installs, or from a
  battery for portable audits.

> [!WARNING]
> KUTSU is an air-quality **awareness** instrument. The MQ-2 reports a *relative* gas/smoke
> level. Do not rely on it as a certified smoke, CO or life-safety alarm.

---

*By Papathought.*
