"""
KUTSU Sense — main entry point (CircuitPython runs code.py on boot).

Flow:
  1. Init sensors on a clean bus (the SGP40 is picky), then bring up the display.
  2. Show welcome/logo, init logging (SD -> flash fallback), NFC, buzzer.
  3. Loop every SAMPLE_INTERVAL_S: read -> history -> evaluate -> display -> log.

Screens toggle with the push button (live <-> charts). A harmful reading shows a
full-screen alarm for ALARM_SHOW_S seconds, then hides for ALARM_GAP_S (showing
normal readings) before it can re-appear — so it never permanently blocks the
display while a harmful condition persists.
"""

import time
import board
import busio

import config
from kutsu import alarms
from kutsu.sensors import Sensors
from kutsu.display import Display
from kutsu.history import History
from kutsu.logging_store import Logger
from kutsu.nfc import NtagWriter
from kutsu.buzzer import Buzzer


def _pin(name):
    return getattr(board, name)


def make_i2c():
    return busio.I2C(_pin(config.I2C_SCL), _pin(config.I2C_SDA),
                     frequency=config.I2C_FREQUENCY)


def setup_logging(logger):
    """Prefer SD card; fall back to internal flash."""
    try:
        spi = busio.SPI(_pin(config.SD_SCK), _pin(config.SD_MOSI), _pin(config.SD_MISO))
        # sdcardio.SDCard wants the raw Pin, not a DigitalInOut
        if logger.mount_sd(spi, _pin(config.SD_CS)):
            return
    except Exception as e:  # noqa: BLE001
        print("[main] SD setup error:", e)
    logger.use_flash()


def make_button():
    try:
        import keypad
        return keypad.Keys((_pin(config.BUTTON_PIN),),
                           value_when_pressed=False, pull=True)
    except Exception as e:  # noqa: BLE001
        print("[main] button init failed:", e)
        return None


def _render_screen(display, screen, reading, ev_result, history):
    """Draw the normal (non-alarm) screen for the current toggle state."""
    if screen == 0:
        display.show_live(reading, ev_result)
    else:
        display.show_charts(history)


def main():
    # Release any display/bus a previous (crashed) run left claimed, so we can
    # re-acquire GP4/GP5 — otherwise make_i2c() fails with "SCL in use".
    import displayio
    displayio.release_displays()

    i2c = make_i2c()

    # Diagnostic: print what's actually on the bus each boot.
    while not i2c.try_lock():
        pass
    try:
        print("[main] I2C scan:", [hex(a) for a in i2c.scan()])
    finally:
        i2c.unlock()

    # Init sensors FIRST, on a clean bus — the SGP40's timing-sensitive init
    # fails ([Errno 19]) if the displayio-managed OLED is already on the bus.
    sensors = Sensors(i2c)

    display = Display(i2c)
    display.show_welcome()

    history = History()
    logger = Logger()
    setup_logging(logger)
    buzzer = Buzzer()
    button = make_button()

    # Provision the fixed NFC health record once.
    try:
        NtagWriter(i2c).provision_health_record()
    except Exception as e:  # noqa: BLE001
        print("[main] NFC provisioning skipped:", e)

    # Hold the welcome screen briefly so it's actually visible.
    time.sleep(2.0)

    screen = 0             # 0 = live, 1 = charts
    alarm_showing = False  # is the full-screen alarm currently on the display?
    alarm_phase_end = 0.0  # monotonic time the current show/gap phase ends
    warning_latched = False  # SOS fires once per warning; latched while harmful
    start_ms = time.monotonic_ns() // 1_000_000
    next_tick = time.monotonic()

    reading = {"voc_index": None, "temp_c": None, "humidity_pct": None}
    ev_result = alarms.evaluate(reading, voc_ready=False)

    while True:
        now = time.monotonic()
        buzzer.update(now)  # advance non-blocking SOS playback

        # --- button: toggle screen (immediate, unless an alarm is on screen) ---
        if button is not None:
            ev = button.events.get()
            if ev and ev.pressed:
                screen ^= 1
                if not alarm_showing:
                    _render_screen(display, screen, reading, ev_result, history)

        if now >= next_tick:
            next_tick += config.SAMPLE_INTERVAL_S
            uptime_s = (time.monotonic_ns() // 1_000_000 - start_ms) / 1000

            reading = sensors.read()
            history.add(reading, uptime_s)
            ev_result = alarms.evaluate(reading, voc_ready=sensors.voc_ready())
            harmful = ev_result["harmful"]

            if harmful:
                if not warning_latched:
                    # transition into a warning -> sound the SOS once
                    warning_latched = True
                    buzzer.start_sos(now)
                if not alarm_showing and now >= alarm_phase_end:
                    # start a fresh 5 s alarm display
                    alarm_showing = True
                    alarm_phase_end = now + config.ALARM_SHOW_S
                    display.show_alarm(harmful)
                elif alarm_showing and now >= alarm_phase_end:
                    # 5 s elapsed -> hide for the gap, show readings meanwhile
                    alarm_showing = False
                    alarm_phase_end = now + config.ALARM_GAP_S
                    _render_screen(display, screen, reading, ev_result, history)
                elif not alarm_showing:
                    # within the gap -> keep refreshing the normal screen
                    _render_screen(display, screen, reading, ev_result, history)
                # else: within the 5 s window -> leave the alarm up untouched
            else:
                # no harmful reading -> normal display, reset alarm + warning latch
                alarm_showing = False
                alarm_phase_end = 0.0
                warning_latched = False
                _render_screen(display, screen, reading, ev_result, history)

            logger.log(reading, uptime_s, bool(harmful))

        time.sleep(0.02)  # keep button polling responsive


main()
