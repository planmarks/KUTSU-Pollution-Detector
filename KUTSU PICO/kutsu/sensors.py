"""
Sensor bring-up and reading: HTU21D (temp/humidity) + SGP40 (VOC index).

The SGP40 VOC index needs temperature + humidity for compensation, so we read
the HTU21D first and feed it into measure_index(). Each sensor degrades
gracefully: if a chip is missing/unwired, its reading comes back as None instead
of crashing the whole device.

Also reads the CCS811 (eCO2 ppm + TVOC ppb, I2C) and the MQ-2 combustible-gas /
smoke sensor (analog, via the ADC). All sensors degrade gracefully.

Requires CircuitPython libraries in /lib:  adafruit_htu21d, adafruit_sgp40,
adafruit_ccs811 (+ adafruit_bus_device, adafruit_register). MQ-2 uses built-in analogio.
"""

import time

try:
    import config
except ImportError:
    from .. import config  # type: ignore


class Sensors:
    def __init__(self, i2c):
        self.i2c = i2c
        self.htu = None
        self.sgp = None
        self._start_ms = time.monotonic_ns() // 1_000_000
        self._last_voc = None   # hold last good VOC through transient bus errors
        self._voc_fails = 0
        self.ccs = None
        self.mq2 = None
        self._last_eco2 = None
        self._last_tvoc = None

        try:
            import adafruit_htu21d
            self.htu = adafruit_htu21d.HTU21D(i2c)
        except Exception as e:  # noqa: BLE001 - report and continue
            print("[sensors] HTU21D init failed:", e)

        try:
            import adafruit_sgp40
            time.sleep(1.0)  # let the SGP40 settle before its read-backs

            # This SGP40 breakout NAKs the factory self-test read (Adafruit lib
            # initialize() line ~149) even though the serial + feature-set reads
            # succeed. The self-test isn't needed to measure VOC, so replace
            # initialize() with a version that keeps those checks but skips it.
            def _init_no_selftest(dev):
                dev._command_buffer[0] = 0x36
                dev._command_buffer[1] = 0x82
                dev._read_word_from_command(3)   # serial number
                dev._command_buffer[0] = 0x20
                dev._command_buffer[1] = 0x2F
                dev._read_word_from_command()    # feature set
                dev._reset()
            adafruit_sgp40.SGP40.initialize = _init_no_selftest

            last_err = None
            for _attempt in range(3):
                try:
                    self.sgp = adafruit_sgp40.SGP40(i2c)
                    break
                except Exception as e:  # noqa: BLE001 - retry slow start-ups
                    last_err = e
                    time.sleep(0.5)
            if self.sgp is None and last_err is not None:
                raise last_err
        except Exception as e:  # noqa: BLE001
            print("[sensors] SGP40 init failed:", e)
            try:
                import traceback  # CP10: use traceback module
                traceback.print_exception(e)
            except Exception:  # noqa: BLE001
                try:
                    import sys
                    sys.print_exception(e)
                except Exception:  # noqa: BLE001
                    pass

        # CCS811 — eCO2 / TVOC over I2C. WAK pin must be grounded (wiring).
        try:
            import adafruit_ccs811
            self.ccs = adafruit_ccs811.CCS811(i2c, address=config.ADDR_CCS811)
        except Exception as e:  # noqa: BLE001
            print("[sensors] CCS811 init failed:", e)

        # MQ-2 — analog combustible-gas / smoke level via ADC + divider.
        try:
            import board
            import analogio
            self.mq2 = analogio.AnalogIn(getattr(board, config.MQ2_AO_PIN))
        except Exception as e:  # noqa: BLE001
            print("[sensors] MQ-2 ADC init failed:", e)

    def voc_ready(self):
        """VOC index is meaningful only after the warm-up window."""
        uptime_s = (time.monotonic_ns() // 1_000_000 - self._start_ms) / 1000
        return uptime_s >= config.VOC_WARMUP_S

    def read(self):
        """Return {voc_index, temp_c, humidity_pct} (any value may be None)."""
        temp_c = humidity = None
        if self.htu is not None:
            try:
                temp_c = float(self.htu.temperature)
                humidity = float(self.htu.relative_humidity)
            except Exception as e:  # noqa: BLE001
                print("[sensors] HTU21D read failed:", e)

        # SGP40 reads can hit transient CRC/bus errors on a marginal bus; retry
        # a few times and otherwise hold the last good value instead of blanking.
        if self.sgp is not None:
            last_e = None
            for _ in range(3):
                try:
                    if temp_c is not None and humidity is not None:
                        self._last_voc = self.sgp.measure_index(
                            temperature=temp_c, relative_humidity=humidity
                        )
                    else:
                        self._last_voc = self.sgp.measure_index()
                    self._voc_fails = 0
                    break
                except Exception as e:  # noqa: BLE001
                    last_e = e
            else:
                self._voc_fails += 1
                if self._voc_fails % 10 == 1:  # throttle console spam
                    print("[sensors] SGP40 read failed (x{}): {}".format(
                        self._voc_fails, last_e))

        # CCS811: eCO2 (ppm) + TVOC (ppb). Refreshes ~1 Hz; hold last when not ready.
        if self.ccs is not None:
            try:
                if self.ccs.data_ready:
                    self._last_eco2 = self.ccs.eco2
                    self._last_tvoc = self.ccs.tvoc
            except Exception as e:  # noqa: BLE001
                print("[sensors] CCS811 read failed:", e)

        # MQ-2: raw 16-bit ADC value (higher = more gas/smoke). Qualitative.
        mq2_raw = None
        if self.mq2 is not None:
            try:
                mq2_raw = self.mq2.value
            except Exception as e:  # noqa: BLE001
                print("[sensors] MQ-2 read failed:", e)

        return {
            "voc_index": self._last_voc,
            "temp_c": round(temp_c, 1) if temp_c is not None else None,
            "humidity_pct": round(humidity, 1) if humidity is not None else None,
            "eco2_ppm": self._last_eco2,
            "tvoc_ppb": self._last_tvoc,
            "mq2_raw": mq2_raw,
        }
