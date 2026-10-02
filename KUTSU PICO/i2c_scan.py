"""
I2C bus bring-up check — run this BEFORE the full firmware.

Copy to the CIRCUITPY drive. Then either:
  * rename/replace this as `code.py` so it runs on boot, OR
  * in the serial REPL (Ctrl-C to stop the running program), type:  import i2c_scan

It scans the I2C bus every 3 s and reports whether each expected device answers.
Expect exactly four addresses. A missing one = wiring / pull-up / power issue on
that device; an unexpected extra = address clash or a stray board.
"""

import time
import board
import busio
import config

EXPECTED = {
    config.ADDR_HTU21D: "HTU21D (temp/humidity)",
    config.ADDR_SGP40: "SGP40 (air quality)",
    config.ADDR_SSD1306: "OLED SSD1306",
    config.ADDR_NTAG: "NFC NTAG",
}

i2c = busio.I2C(getattr(board, config.I2C_SCL), getattr(board, config.I2C_SDA),
                frequency=config.I2C_FREQUENCY)

while True:
    while not i2c.try_lock():
        pass
    try:
        found = i2c.scan()
    finally:
        i2c.unlock()

    print("\nI2C scan @ {} Hz -> {}".format(
        config.I2C_FREQUENCY, [hex(a) for a in found]))
    for addr, name in EXPECTED.items():
        mark = "  OK " if addr in found else " MISS"
        print("  {} {:#04x}  {}".format(mark, addr, name))
    extra = [hex(a) for a in found if a not in EXPECTED]
    if extra:
        print("  ?? unexpected:", extra)
    time.sleep(3)
