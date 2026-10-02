# KUTSU DIY

> The **solder-it-yourself** generation of KUTSU Sense. There are two dedicated boards, one
> for carrying around and one for fixing to a wall, and both are built from widely available
> breakout modules.

[← Back to project overview](../README.md)

<p align="center">
  <img src="KUTSU%20Handheld/KUTSU-Handheld-Test-Render.png" alt="KUTSU Handheld board" width="45%">
  <img src="KUTSU%20Stationary/KUTSU-Stationary-test-render.png" alt="KUTSU Stationary board" width="45%">
</p>

> [!NOTE]
> These builds came before **[KUTSU 1.0](../KUTSU%201.0/README.md)**, which merges both onto a
> single board with Sensirion sensors. The DIY designs and firmware are kept here for makers
> and existing deployments.

---

## The two variants

| | ✋ [**KUTSU Handheld**](KUTSU%20Handheld/README.md) | 🏠 [**KUTSU Stationary**](KUTSU%20Stationary/README.md) |
|---|---|---|
| **Purpose** | Walk-up, on-site spot checks | Continuous monitoring of a room or site |
| **MCU** | Seeed XIAO ESP32-C3 or ESP32-C5 | Espressif ESP32-C5-DevKitC-1 v1.2 |
| **PCB** | KUTSU Sense **v0.4P** (2-layer) | KUTSU Sense **v0.3S** (2-layer) |
| **Power** | LiPo battery (USB-C charging via the XIAO) | 5 V mains / USB |
| **Air quality** | CCS811 (eCO₂ + TVOC) | CCS811 (eCO₂ + TVOC) **+ MQ-2 gas/smoke** |
| **Temp / RH** | HTU21D | HTU21D |
| **User feedback** | OLED + buzzer | OLED + buzzer + **green/yellow/red LEDs** (OLED optional) |
| **Fan** | Small 3 V fan | 5 V fan |
| **Logging** | CSV → MicroSD / flash | **JSON Lines** → MicroSD / flash |
| **Radios** | Passive Wi-Fi + BLE scanner screens | **BLE app link + Wi-Fi mesh with web dashboard** |

---

## Shared sensor set

| Sensor | Measures | Interface | I²C address | Notes |
|---|---|---|---|---|
| **CCS811** | eCO₂ (ppm), TVOC (ppb) | I²C | `0x5A` | **Estimated** CO₂; needs ~20 min run-in / 48 h burn-in; `WAK → GND` is required |
| **HTU21D-F** | Temperature, humidity | I²C | `0x40` | Calibrated absolute values |
| **SSD1306 OLED** 0.96" | Display | I²C | `0x3C` | 128 × 64 |
| **MicroSD** | Logging | SPI | — | Adafruit #4682 breakout or on-board socket |
| **MQ-2** *(Stationary only)* | Combustible gas / smoke | Analog | — | 5 V part, ~150 mA heater |

---

## Shared firmware behaviour

Both variants run the same Arduino/C++ core loop:

- **1 Hz tick:** `readSensors()` → `historyAdd()` → `evaluate()` → render or alarm → `logRow()`
- **Severity:** each metric is graded *good* / *warning* `!` / *alert*
- **Alarm:** a full-screen WARNING pulses (5 s on, 1 s gap) and the buzzer beeps 3 times;
  press the button to dismiss
- **History:** 60 one-minute bins for the one-hour chart screen

| Metric | Warning `!` | Alert |
|---|---|---|
| eCO₂ | ≥ 900 ppm | ≥ 2000 ppm |
| TVOC | ≥ 660 ppb | ≥ 3000 ppb |
| Temperature | 50–60 °C | < 0 °C or > 60 °C |
| Humidity | 25–30 % or 60–70 %RH | < 25 % or > 70 %RH |
| Gas (MQ-2, Stationary) | raw ≥ 1800 | raw ≥ 2600 *(calibrate)* |
