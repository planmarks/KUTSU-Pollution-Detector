"""
Data logging: SD card if present, otherwise an internal-flash ring buffer.

- If the MicroSD breakout mounts, append rows to SD_LOG_PATH (effectively unbounded).
- Otherwise append to FLASH_LOG_PATH. When that file exceeds FLASH_LOG_MAX_BYTES we
  drop the oldest rows (chronological deletion) by rewriting the tail.

NOTE: writing to the Pico's internal flash requires the filesystem to be mounted
read-write from CircuitPython's side. See boot.py — it remounts "/" writable.
Requires CircuitPython library in /lib:  adafruit_sdcard  (built-ins: storage, sdcardio/busio)
"""

import os

try:
    import config
except ImportError:
    from .. import config  # type: ignore


class Logger:
    def __init__(self):
        self.path = None
        self.on_sd = False
        self._buf = []                 # rows queued if no writable target
        self._writable = False

    # --- target selection ----------------------------------------------------
    def mount_sd(self, spi, cs_pin):
        """Try to mount the SD card. Returns True on success."""
        try:
            import storage
            import sdcardio
            sd = sdcardio.SDCard(spi, cs_pin, baudrate=config.SD_BAUDRATE)
            vfs = storage.VfsFat(sd)
            storage.mount(vfs, "/sd")
            self.path = config.SD_LOG_PATH
            self.on_sd = True
            self._writable = True
            self._ensure_header()
            print("[log] SD card mounted ->", self.path)
            return True
        except Exception as e:  # noqa: BLE001
            print("[log] no SD card / mount failed:", e)
            return False

    def use_flash(self):
        """Fall back to internal flash (requires rw remount via boot.py)."""
        self.path = config.FLASH_LOG_PATH
        self.on_sd = False
        try:
            self._ensure_header()
            self._writable = True
            print("[log] logging to flash ->", self.path)
        except OSError as e:
            # Filesystem is read-only (boot.py didn't remount). Keep rows in RAM.
            self._writable = False
            print("[log] flash read-only, buffering in RAM:", e)

    # --- writing -------------------------------------------------------------
    def log(self, reading, uptime_s, alarm_flag):
        row = "{},{},{},{},{}\n".format(
            int(uptime_s),
            _csv(reading.get("voc_index")),
            _csv(reading.get("temp_c")),
            _csv(reading.get("humidity_pct")),
            1 if alarm_flag else 0,
        )
        if not self._writable:
            self._buf.append(row)
            return
        try:
            with open(self.path, "a") as f:
                f.write(row)
            if not self.on_sd:
                self._enforce_flash_cap()
        except OSError as e:
            print("[log] write failed:", e)

    # --- helpers -------------------------------------------------------------
    def _ensure_header(self):
        try:
            os.stat(self.path)
        except OSError:
            with open(self.path, "w") as f:
                f.write(config.LOG_CSV_HEADER + "\n")

    def _enforce_flash_cap(self):
        try:
            size = os.stat(self.path)[6]
        except OSError:
            return
        if size <= config.FLASH_LOG_MAX_BYTES:
            return
        # Drop the oldest ~25% of rows, keep header.
        with open(self.path, "r") as f:
            lines = f.readlines()
        header, body = lines[0], lines[1:]
        keep = body[len(body) // 4:]
        with open(self.path, "w") as f:
            f.write(header)
            f.writelines(keep)
        print("[log] flash cap hit, trimmed oldest rows")


def _csv(v):
    return "" if v is None else v
