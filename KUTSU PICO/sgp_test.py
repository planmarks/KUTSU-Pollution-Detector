"""
Direct SGP40 test — bypasses all firmware layers so the REAL traceback prints.

Run it by temporarily copying it OVER code.py on the board:
  1. (Your real code.py is safe in the project folder — you'll restore it after.)
  2. Copy this file's contents to CIRCUITPY/code.py
  3. Reboot and read the serial output.
  4. Restore the real code.py afterward.

On a fresh boot the I2C bus is free, so there's no "SCL in use", and the SGP40
constructor is allowed to raise naturally -> full traceback pointing at the exact
command inside adafruit_sgp40 that the sensor NAKs.
"""

import time
import board
import busio
import adafruit_sgp40

print("=== SGP40 direct test ===")

i2c = busio.I2C(board.GP5, board.GP4, frequency=100000)
while not i2c.try_lock():
    pass
print("scan:", [hex(a) for a in i2c.scan()])
i2c.unlock()

time.sleep(1)
print("constructing SGP40 (no error handling — real traceback follows if it fails)...")
sgp = adafruit_sgp40.SGP40(i2c)
print("SGP40 constructed OK.")
print("VOC index:", sgp.measure_index(temperature=22, relative_humidity=45))
