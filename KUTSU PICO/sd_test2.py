"""
Alternative MicroSD test using adafruit_sdcard (instead of the built-in sdcardio).
Different init timing — sometimes mounts a card that sdcardio times out on.

Needs in /lib:  adafruit_sdcard.mpy  and  adafruit_bus_device/
(from the CircuitPython 10.x library bundle).

Run by copying over code.py, reboot, read output, then restore code.py.
Note: adafruit_sdcard wants cs as a DigitalInOut (sdcardio wanted a raw Pin).
"""

import board
import busio
import digitalio
import storage
import os
import config
import adafruit_sdcard

print("=== MicroSD test (adafruit_sdcard) ===")
print("pins: SCK={} MOSI={} MISO={} CS={}".format(
    config.SD_SCK, config.SD_MOSI, config.SD_MISO, config.SD_CS))

spi = busio.SPI(getattr(board, config.SD_SCK),
                getattr(board, config.SD_MOSI),
                getattr(board, config.SD_MISO))
cs = digitalio.DigitalInOut(getattr(board, config.SD_CS))

try:
    sd = adafruit_sdcard.SDCard(spi, cs, baudrate=400_000)   # slow for reliability
    vfs = storage.VfsFat(sd)
    storage.mount(vfs, "/sd")
    print("MOUNTED OK. Root of card:", os.listdir("/sd"))
    with open("/sd/kutsu_test.txt", "w") as f:
        f.write("hello from KUTSU Sense\n")
    print("WROTE /sd/kutsu_test.txt OK")
except Exception as e:  # noqa: BLE001
    print("SD FAILED:", e)
    try:
        import traceback
        traceback.print_exception(e)
    except Exception:  # noqa: BLE001
        pass
