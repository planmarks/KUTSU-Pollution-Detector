# KUTSU Sense — Prototype Feature Sheet

[← KUTSU PICO](README.md)

An in-depth environmental awareness and monitoring device.

## Features

| Feature | Status in firmware |
|---|---|
| 🌡️ Temperature & humidity monitoring (HTU21D) | ✅ |
| 🌫️ Wide-range air-quality monitoring (SGP40, CCS811 & MQ-2) | ✅ (CCS811 and MQ-2 are display-only) |
| 🖥️ Real-time monitoring display | ✅ |
| 🔊 Audio danger alerts | ✅ Morse SOS |
| 💾 Data logging & MicroSD card support | ✅ |
| 🔦 Red & white flashlight | Planned |
| 🔴 Red-dot laser | Planned |
| 🔋 Rechargeable | ✅ Pico LiPo charger |

## Sensors

| Sensor | Measures |
|---|---|
| HTU21D | Temperature, humidity |
| SGP40 | VOC index |
| CCS811 | eCO₂, TVOC |
| MQ-2 | Combustible gas, smoke |

## Display & audio

- 0.96" 128 × 64 px black-and-white OLED display
- Active buzzer

## Controls

- Device on/off switch
- Flashlight switch: Mode 1 · Off · Mode 2
- Laser tactile switch
- Display page switch

## MCU

Raspberry Pi RP2040 on a **Pimoroni Pico LiPo (PIM560)**.
