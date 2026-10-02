"""
Direct MicroSD (SPI) test — isolates the card from the rest of the firmware.

Run by copying this over code.py on the board (your real code.py is safe in the
project folder). Reboot and read the serial output, then restore code.py.

Mounts at a low baud rate for reliability during bring-up, lists the card, and
writes a test file. A full traceback prints if anything fails.
"""

import board
import busio
import storage
import sdcardio
import os
import config

print("=== MicroSD SPI test ===")
print("pins: SCK={} MOSI={} MISO={} CS={}".format(
    config.SD_SCK, config.SD_MOSI, config.SD_MISO, config.SD_CS))

spi = busio.SPI(getattr(board, config.SD_SCK),
                getattr(board, config.SD_MOSI),
                getattr(board, config.SD_MISO))
cs = getattr(board, config.SD_CS)

try:
    sd = sdcardio.SDCard(spi, cs, baudrate=1_000_000)   # slow = tolerant
    vfs = storage.VfsFat(sd)
    storage.mount(vfs, "/sd")
    print("MOUNTED OK. Root of card:", os.listdir("/sd"))
    with open("/sd/kutsu_test.txt", "w") as f:
        f.write("hello from KUTSU Sense\n")
    print("WROTE /sd/kutsu_test.txt OK")
    print("Re-listing:", os.listdir("/sd"))
except Exception as e:  # noqa: BLE001
    print("SD FAILED:", e)
    try:
        import traceback
        traceback.print_exception(e)
    except Exception:  # noqa: BLE001
        pass
