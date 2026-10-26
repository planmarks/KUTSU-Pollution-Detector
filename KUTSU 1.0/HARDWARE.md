# KUTSU 1.0 — Hardware

[← KUTSU 1.0](README.md) · [Firmware →](KutsuFirmwareXIAOESP32Series/README.md)

The **Kutsu 1 board, rev 3**: a single 4-layer PCB that carries the MCU, the full Sensirion
sensor stack, the display, the MicroSD socket and the user controls. Units ship
**pre-soldered and enclosed**, ready to power on.

![Kutsu 1 PCB render](3D/Kutsu-PCB-Render.png)

---

## Board at a glance

| | |
|---|---|
| **Title / revision** | Kutsu 1 · rev 3 (2026-09-22) |
| **EDA tool** | KiCad 10 |
| **Size** | 56.9 × 78.7 mm |
| **Layers** | 4 (F.Cu · In1.Cu · In2.Cu · B.Cu), 1.6 mm |
| **MCU** | Seeed **XIAO ESP32** module: **C6** (reference fit), **C3**, **C5** or **S3**. Same footprint, one firmware |
| **Sensor bus** | One shared I²C bus: SCD41 `0x62` · SHT40 `0x44` · SGP41 `0x59` · OLED `0x3C` |
| **Storage** | MicroSD socket over SPI |
| **Power** | USB-C 5 V, or a LiPo cell on the battery header with a slide power switch |

---

## Bill of materials

### Active parts and modules

| Ref | Part | Function |
|---|---|---|
| U4 | **Seeed XIAO ESP32-C6** *(or C3 / C5 / S3; symbol reads `XIAO-ESP32-C5-SMD`; same footprint)* | Microcontroller + radios |
| U3 | **Sensirion SCD41-D-R2** | True NDIR CO₂ (+ T/RH) |
| U1 | **Sensirion SGP41-D-R4** | VOC + NOx raw signals → gas indices |
| U2 | **Sensirion SHT40-AD1B-R3** | Precision temperature + humidity |
| DS1 | **0.96" 128×64 OLED panel** (OLED-128O064D / DLC0096DNOG-W-5, SSD1306-class, I²C mode) | Display. A plug-in SSD1306 module (DLC0096DNOG-W-10) works with the same firmware |
| BZ1 | Magnetic SMT transducer (PUI SMT-1028-T-2-R, 18 Ω, ~2.7 kHz) | Audible alarm. Needs a square-wave drive, which the firmware provides |
| D1 | LED (0805) | Severity / warning indicator |
| LD1 | Laser diode | Point-of-interest marker |
| J1 | CUI **MSD-11-A** MicroSD socket | Data logging |

### Controls and connectors

| Ref | Part | Function |
|---|---|---|
| SW2 | Tactile push button | **BTN1**: cycle screens / dismiss alarm |
| SW3 | Tactile push button | **BTN2**: reserved |
| SW1 | Tactile push button | **Laser**: switches 3V3 straight to LD1 (no firmware involvement) |
| SW4 | SPDT slide switch (C&K OS102011MA1Q) | Power on/off |
| BT1 | 2-pin header | LiPo battery |
| MQ2 | 4-pin header: AO · (n/c) · GND · 5V | Optional external MQ-2 gas/smoke module |
| FAN | 2-pin header on the **5 V** rail | Airflow fan |

### Passives

| Ref | Value | Purpose |
|---|---|---|
| R2 / R1 | 10 kΩ / 20 kΩ | MQ-2 analog divider: 5 V output scaled to ≈ 3.3 V for the ADC |
| R5 | 390 kΩ | OLED panel IREF (segment current reference) |
| R13 / R14 | 4.7 kΩ | I²C SDA / SCL pull-ups to 3V3 |
| R6–R11 | 4.7 kΩ | MicroSD line pull-ups (CLK, MOSI, MISO, CS, DAT1, DAT2) |
| R12 | 4.7 kΩ | MicroSD card-detect pull-up |
| R15 | 4.7 kΩ | Warning-LED series resistor |
| R3 / R4 | 4.7 kΩ | Second SCL / SDA pull-ups, in parallel with R14/R13 (≈ 2.35 kΩ per line) |
| C1–C12 | 0.1 µF – 10 µF (0805) | Decoupling + OLED charge-pump capacitors |

---

## Pin map

The table below was checked against the rev 3 schematic (netlist re-exported from
`kutsu-10-xiao-esp32c5c3.kicad_sch`). The XIAO pad labels (`D0`–`D10`) are identical across the
C3, C5, C6 and S3 modules. The firmware uses the pad names, and each module's Arduino variant
maps them to GPIOs.

| XIAO pad | C3 | C5 | C6 | S3 | Net | Function |
|---|---|---|---|---|---|---|
| D0 | GPIO2 | GPIO1 | GPIO0 | GPIO1 | `MQ2AO` | MQ-2 analog (through the R2/R1 divider). Read when a module is fitted |
| D1 | GPIO3 | GPIO0 | GPIO1 | GPIO2 | `D1` | Warning LED (active-high) |
| D2 | GPIO4 | GPIO25 | GPIO2 | GPIO3 | `GPIO4` | Buzzer |
| D3 | GPIO5 | GPIO7 | GPIO21 | GPIO4 | `SDCS` | MicroSD CS |
| D4 | GPIO6 | GPIO23 | GPIO22 | GPIO5 | `SDA` | I²C data |
| D5 | GPIO7 | GPIO24 | GPIO23 | GPIO6 | `SCL` | I²C clock |
| D6 | GPIO21 | GPIO11 | GPIO16 | GPIO43 | `BTN1` | Button 1 (active-low, internal pull-up). Also UART0 TX |
| D7 | GPIO20 | GPIO12 | GPIO17 | GPIO44 | `BTN2` | Button 2 (active-low, internal pull-up). Also UART0 RX |
| D8 | GPIO8 | GPIO8 | GPIO19 | GPIO7 | `SDCLK` | MicroSD SCK |
| D9 | GPIO9 | GPIO9 | GPIO20 | GPIO8 | `SDSO` | MicroSD MISO |
| D10 | GPIO10 | GPIO10 | GPIO18 | GPIO9 | `SDSI` | MicroSD MOSI |
| 3V3 / GND / 5V | — | `+3V3` / `GND` / `5V` | Power rails |
| BAT pads (underside) | — | `BATTERY` | LiPo + charger |
| *(C6 internal)* | | | GPIO3 | | — | **RF-switch enable**: driven LOW to power the C6 RF switch |
| *(C6 internal)* | | | GPIO14 | | — | **Antenna select**: LOW = on-board, HIGH = external u.FL |

> [!IMPORTANT]
> **D6/D7 are UART0 TX/RX on every module**, so firmware must be built with **USB CDC On Boot
> enabled**. The firmware enforces this at compile time.

> [!TIP]
> **Strapping pins per module:**
> - **C6 / S3:** D8/D9 are not strapping pins.
> - **C3:** D8/D9 (GPIO8/9) *are* strapping pins, but the SD pull-ups R6/R8 hold them at the
>   normal-boot level.
> - **C5:** D2 is GPIO25, a strapping pin, and BZ1's coil loads it. Confirm normal boot on the
>   first C5 build.

---

## Block diagram

```mermaid
flowchart LR
    USB[USB-C 5V] --> XIAO
    BAT[LiPo · BT1] --> SW4[SW4 power] --> XIAO
    subgraph XIAO[XIAO ESP32-C3 / C5 / C6 / S3]
      I2C[D4 SDA / D5 SCL]
      SPI[D8 · D9 · D10 · D3]
      GP[D0 · D1 · D2 · D6 · D7]
    end
    I2C --> SCD41[SCD41 CO2 0x62]
    I2C --> SGP41[SGP41 VOC/NOx 0x59]
    I2C --> SHT40[SHT40 T/RH 0x44]
    I2C --> OLED[OLED 0x3C]
    SPI --> SD[MicroSD J1]
    GP --> BZ[Buzzer]
    GP --> LED[Warning LED]
    GP --> BTN[BTN1 / BTN2]
    MQ2[MQ-2 module] -- 10k/20k divider --> GP
    SW1[SW1] --> LASER[Laser LD1]
```

---

## Power

| Source | Use |
|---|---|
| **USB-C** (on the XIAO) | Powers the board at 5 V and charges a connected LiPo |
| **LiPo on BT1** | Handheld, untethered use; switched by SW4 |
| **Continuous 5 V** | Stationary use: needed for the **MQ-2** heater (~150 mA) and a **5 V fan** |

The FAN and MQ2 headers both sit on the `5V` rail, which is only present when USB-C is connected.

> [!IMPORTANT]
> The MQ-2 heater draws about 150 mA at 5 V, which is too much for battery operation. Fit the
> MQ-2 only in the Stationary configuration.

---

## Enclosure

The enclosure (see `Case/Kutsu 1 Case.blend`) has a display window, three front buttons, side
vents, a USB-C port and an **external whip antenna**. When you fit the external antenna, set
`RF_USE_EXTERNAL_ANT 1` in the firmware's `config.h`.

![Enclosure render](Case/Render.png)

---

## Design files

| File | Purpose |
|---|---|
| `kutsu-10-xiao-esp32c5c3.kicad_pro` | KiCad project |
| `kutsu-10-xiao-esp32c5c3.kicad_sch` | Schematic, **rev 3** (authoritative) |
| `kutsu-10-xiao-esp32c5c3.kicad_pcb` | PCB layout, 4-layer |
| `kutsu-10-xiao-esp32c5c6.net` | Netlist. **Stale:** it's the rev 2 export (identical to the Handheld netlist). Re-export from the rev 3 schematic before relying on it |
| `KUTSUV1 Gerber/`, `KUTSUV1 Gerber.zip` | Fabrication outputs: copper (4 layers), mask, paste, silk, edge cuts, PTH/NPTH drill, job file |
| `3D/kutsu-board-*.stl` / `.glb` | 3D board models |
| `Case/Kutsu 1 Case.blend` | Enclosure model (Blender) |

> [!NOTE]
> The project files are named `…c5c3` / `…c5c6` and the MCU symbol reads `XIAO-ESP32-C5-SMD`.
> That naming is historical. The reference module is the **XIAO ESP32-C6**. The C3, C5 and S3
> have an identical pad layout and run the same firmware.

### Ordering PCBs

Upload `KUTSUV1 Gerber.zip` to your PCB fab and choose **4 layers, 1.6 mm**. The job file
(`*-job.gbrjob`) carries the stack-up and board-size metadata.

---

## Known hardware notes

- **OLED panel (DS1).**
  - Wired for I²C mode: BS1 = 3V3, BS0/BS2 = GND.
  - Address `0x3C`: D/C# (SA0) is tied to GND.
  - Internal charge pump: C2/C3, VBAT on 3V3.
  - **`RES#` is tied directly to 3V3**, so there's no reset pulse at power-up. The firmware
    re-initialises the controller on every boot.
  - For the next revision, add an RC delay on `RES#` (e.g. 10 kΩ / 1 µF) or route it to a GPIO.
- **Buzzer drive.** BZ1 is an 18 Ω magnetic transducer wired straight to D2 with no transistor.
  The firmware drives it with a PWM tone. Loudness is limited by the GPIO's drive strength, so
  add an NPN/MOSFET driver and a flyback diode in the next revision.
- **RF switch (C6).** If Wi-Fi or BLE can't find anything, check `RF_USE_EXTERNAL_ANT` in
  `config.h` against the antenna actually fitted.
- **MQ-2 input (D0).** Read as a raw, relative level. The R2/R1 divider pulls D0 to ~0 when no
  module is fitted, so the firmware hides the metric until it sees a valid reading.
