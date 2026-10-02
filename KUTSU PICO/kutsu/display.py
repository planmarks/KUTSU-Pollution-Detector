"""
OLED UI: welcome, live readings, 1-hour charts, and full-screen alarm.

Built on displayio + the SSD1306 driver. Screens are pre-built displayio Groups
that we swap onto the display root; per-frame we only mutate label text / chart
bitmaps, which keeps refresh cheap.

Requires CircuitPython libraries in /lib:  adafruit_displayio_ssd1306
(and the built-in displayio, terminalio, vectorio).
"""

import displayio
import terminalio
from adafruit_display_text import label

try:
    import config
except ImportError:
    from .. import config  # type: ignore

# Display range used to scale each metric's chart bars (min, max).
CHART_RANGES = {
    "voc_index": (0, 500),
    "temp_c": (0, 40),
    "humidity_pct": (0, 100),
}
CHART_ORDER = ("voc_index", "temp_c", "humidity_pct")
SHORT_LABEL = {"voc_index": "VOC", "temp_c": "TEMP", "humidity_pct": "HUM"}

# Live screen rows: (reading key, label, unit suffix). Six rows fit a 128x64 OLED.
LIVE_ROWS = (
    ("voc_index", "VOC", "idx"),
    ("eco2_ppm", "CO2", "ppm"),
    ("tvoc_ppb", "TVOC", "ppb"),
    ("mq2_raw", "GAS", ""),
    ("temp_c", "TEMP", "C"),
    ("humidity_pct", "HUM", "%"),
)

WHITE = 0xFFFFFF
BLACK = 0x000000


class Display:
    def __init__(self, i2c):
        displayio.release_displays()
        import adafruit_displayio_ssd1306
        from i2cdisplaybus import I2CDisplayBus  # CP 9+: moved out of displayio
        bus = I2CDisplayBus(i2c, device_address=config.ADDR_SSD1306)
        self.oled = adafruit_displayio_ssd1306.SSD1306(
            bus, width=config.OLED_WIDTH, height=config.OLED_HEIGHT
        )
        # Manual refresh only: with a shared I2C bus, background auto-refresh
        # interrupts the SGP40's timing-sensitive init/read (-> [Errno 19]).
        self.oled.auto_refresh = False
        self.W = config.OLED_WIDTH
        self.H = config.OLED_HEIGHT
        self._build_live()
        self._build_chart()

    # --- Welcome -------------------------------------------------------------
    def show_welcome(self):
        g = displayio.Group()
        try:
            bmp = displayio.OnDiskBitmap("/assets/welcome_logo.bmp")
            g.append(displayio.TileGrid(bmp, pixel_shader=bmp.pixel_shader))
        except Exception as e:  # noqa: BLE001 - fall back to text
            print("[display] logo load failed:", e)
            g.append(label.Label(terminalio.FONT, text="KUTSU Sense",
                                  color=WHITE, x=20, y=self.H // 2))
        self.oled.root_group = g
        self.oled.refresh()

    # --- Live readings -------------------------------------------------------
    def _build_live(self):
        self._live_group = displayio.Group()
        self._live_rows = {}
        y = 5
        for metric, _lbl, _unit in LIVE_ROWS:
            row = label.Label(terminalio.FONT, text="", color=WHITE, x=2, y=y)
            self._live_group.append(row)
            self._live_rows[metric] = row
            y += 10

    def show_live(self, reading, evaluation):
        per = evaluation["per_metric"]
        for metric, lbl, unit in LIVE_ROWS:
            v = reading.get(metric)
            vs = "--" if v is None else str(v)
            sev = per.get(metric, {}).get("severity", 0)  # 0 for metrics w/o thresholds
            flag = "" if sev == 0 else (" !" if sev == 1 else " !!")
            self._live_rows[metric].text = "{}: {}{}{}".format(lbl, vs, unit, flag)
        self.oled.root_group = self._live_group
        self.oled.refresh()

    # --- Charts --------------------------------------------------------------
    def _build_chart(self):
        self._chart_group = displayio.Group()
        # one full-screen 2-colour bitmap holds all bars
        self._chart_bmp = displayio.Bitmap(self.W, self.H, 2)
        pal = displayio.Palette(2)
        pal[0] = BLACK
        pal[1] = WHITE
        self._chart_group.append(displayio.TileGrid(self._chart_bmp, pixel_shader=pal))
        # one small label per chart band
        self._chart_labels = {}
        band = self.H // len(CHART_ORDER)
        for i, metric in enumerate(CHART_ORDER):
            lbl = label.Label(terminalio.FONT, text=SHORT_LABEL[metric],
                              color=WHITE, x=0, y=i * band + 4)
            self._chart_group.append(lbl)
            self._chart_labels[metric] = lbl

    def show_charts(self, history):
        self._chart_bmp.fill(0)
        band = self.H // len(CHART_ORDER)
        bar_w = 2                     # 60 bins * 2px = 120px
        x0 = self.W - config.CHART_BINS * bar_w
        for i, metric in enumerate(CHART_ORDER):
            top = i * band
            plot_top = top + 9        # leave room for the label row
            plot_h = band - 10
            lo, hi = CHART_RANGES[metric]
            span = (hi - lo) or 1
            series = history.series(metric)
            # newest non-None value (CircuitPython lacks next(it, default) & reversed)
            cur = None
            for v in series:
                if v is not None:
                    cur = v
            self._chart_labels[metric].text = "{} {}".format(
                SHORT_LABEL[metric], "--" if cur is None else round(cur)
            )
            for b, v in enumerate(series):
                if v is None:
                    continue
                frac = max(0.0, min(1.0, (v - lo) / span))
                h = int(frac * plot_h)
                x = x0 + b * bar_w
                for yy in range(plot_top + plot_h - h, plot_top + plot_h):
                    if 0 <= yy < self.H:
                        self._chart_bmp[x, yy] = 1
                        if x + 1 < self.W:
                            self._chart_bmp[x + 1, yy] = 1
        self.oled.root_group = self._chart_group
        self.oled.refresh()

    # --- Full-screen alarm ---------------------------------------------------
    def show_alarm(self, harmful):
        """harmful: list of dicts from alarms.evaluate()['harmful']."""
        g = displayio.Group()
        g.append(label.Label(terminalio.FONT, text="!! WARNING !!",
                             color=WHITE, x=18, y=8))
        item = harmful[0]
        msg = item["message"].split("\n")
        y = 26
        for line in msg:
            g.append(label.Label(terminalio.FONT, text=line, color=WHITE, x=4, y=y))
            y += 12
        val = item.get("value")
        if val is not None:
            g.append(label.Label(terminalio.FONT,
                                 text="{}: {}".format(item["label"], val),
                                 color=WHITE, x=4, y=y))
        self.oled.root_group = g
        self.oled.refresh()
