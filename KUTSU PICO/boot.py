"""
boot.py runs once, before code.py, and is the ONLY place CircuitPython can make
the internal flash filesystem writable to the firmware (for flash logging when no
SD card is present).

Trade-off: when "/" is mounted read-write to CircuitPython, the CIRCUITPY USB
drive becomes READ-ONLY to your computer. To edit code over USB again, either
remove this remount or hold the button at boot (see below) to skip it.

If you only ever log to SD, you can delete this file.
"""

import board
import digitalio
import storage

# Hold the screen-toggle button during power-on to KEEP the USB drive writable
# (skips the remount) so you can edit files. Otherwise flash logging is enabled.
SKIP_PIN = board.GP15  # must match config.BUTTON_PIN

skip = digitalio.DigitalInOut(SKIP_PIN)
skip.switch_to_input(pull=digitalio.Pull.UP)
button_held = not skip.value  # active-low

if not button_held:
    try:
        storage.remount("/", readonly=False)
        print("[boot] flash mounted read-write for logging")
    except Exception as e:  # noqa: BLE001
        print("[boot] remount failed:", e)
else:
    print("[boot] button held — leaving USB drive writable, flash logging off")

skip.deinit()
