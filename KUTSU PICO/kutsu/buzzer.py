"""
Active buzzer (built-in oscillator) — plays a morse SOS once per warning.

An active buzzer just needs its pin driven HIGH to sound and LOW to silence
(no PWM / frequency needed). Playback is NON-BLOCKING: start_sos() kicks it off
and update(now) — called every main-loop iteration — steps through the pattern
so the display, button and sensor reads stay responsive.

Wiring (config.BUZZER_PIN, default GP14): active buzzers are polarised.
  * Low current (<=~10 mA): buzzer + -> GP14, buzzer - -> GND.
  * Higher current: drive through an NPN/MOSFET low-side switch (buzzer + -> 3V3,
    buzzer - -> collector, GP14 -> ~1k -> base, emitter -> GND) with a flyback
    diode across the buzzer. Either way the firmware just toggles GP14.
"""

try:
    import config
except ImportError:
    from .. import config  # type: ignore

# Morse timing (seconds). Standard ratios: dash = 3x dot.
_UNIT = 0.15
_DOT = _UNIT
_DASH = 3 * _UNIT
_GAP = _UNIT             # gap between elements within a letter
_LETTER_GAP = 3 * _UNIT  # gap between letters


def _sos_sequence():
    """Build SOS ( . . .   - - -   . . . ) as a list of (level, duration)."""
    letters = ((_DOT, _DOT, _DOT), (_DASH, _DASH, _DASH), (_DOT, _DOT, _DOT))
    seq = []
    for i, letter in enumerate(letters):
        for j, elem in enumerate(letter):
            seq.append((True, elem))                 # tone on
            if j < len(letter) - 1:
                seq.append((False, _GAP))            # gap between elements
        if i < len(letters) - 1:
            seq.append((False, _LETTER_GAP))         # gap between letters
    return seq


class Buzzer:
    def __init__(self, pin_name=None):
        self._out = None
        self._seq = _sos_sequence()
        self._idx = None         # None = idle; otherwise index into _seq
        self._seg_end = 0.0
        try:
            import board
            import digitalio
            self._out = digitalio.DigitalInOut(getattr(board, pin_name or config.BUZZER_PIN))
            self._out.switch_to_output(value=False)
        except Exception as e:  # noqa: BLE001
            print("[buzzer] init failed:", e)

    def start_sos(self, now):
        """Begin one SOS. Safe to call again only after it finishes / on a new warning."""
        if self._out is None:
            return
        self._idx = 0
        level, dur = self._seq[0]
        self._out.value = level
        self._seg_end = now + dur

    def update(self, now):
        """Advance playback; call every loop iteration. No-op when idle."""
        if self._idx is None or self._out is None:
            return
        if now >= self._seg_end:
            self._idx += 1
            if self._idx >= len(self._seq):
                self._out.value = False
                self._idx = None
                return
            level, dur = self._seq[self._idx]
            self._out.value = level
            self._seg_end = now + dur

    def is_playing(self):
        return self._idx is not None

    def off(self):
        self._idx = None
        if self._out is not None:
            self._out.value = False
