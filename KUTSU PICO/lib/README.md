# CircuitPython Libraries

[← KUTSU PICO firmware](../FIRMWARE.md)

This folder holds the libraries the firmware needs, taken from the **CircuitPython 10.x Library
Bundle**. Copy the whole folder to `CIRCUITPY/lib`.

> [!IMPORTANT]
> `.mpy` files are tied to the CircuitPython major version. If you flash a different version,
> replace these files with the matching bundle from <https://circuitpython.org/libraries>.

| Library (folder / file) | Used by | Purpose |
|---|---|---|
| `adafruit_htu21d.mpy` | `kutsu/sensors.py` | Temperature + humidity |
| `adafruit_sgp40/` | `kutsu/sensors.py` | VOC index, including Sensirion's gas-index algorithm |
| `adafruit_ccs811.mpy` | `kutsu/sensors.py` | eCO₂ + TVOC |
| `adafruit_bus_device/` | CCS811, SGP40, `adafruit_sdcard` | I²C/SPI device helpers |
| `adafruit_register/` | `adafruit_ccs811` | Register helpers |
| `adafruit_displayio_ssd1306.mpy` | `kutsu/display.py` | OLED driver |
| `adafruit_display_text/` | `kutsu/display.py` | On-screen text labels |
| `adafruit_sdcard.mpy` | `sd_test2.py` (alternative SD driver) | MicroSD over SPI |

**Built into the CircuitPython firmware** (nothing to copy): `board`, `busio`, `digitalio`,
`analogio`, `displayio`, `terminalio`, `storage`, `keypad`, `sdcardio`.

The **NT3H2111 NFC tag** has no bundled driver, so `kutsu/nfc.py` writes the NDEF data by hand.
