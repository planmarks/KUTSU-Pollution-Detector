"""
Rolling 1-hour history for the chart screen.

Per metric we keep CHART_BINS (60) one-minute averages. The in-progress minute
is accumulated as a running mean and committed to the ring buffer on rollover.
"""

try:
    import config
except ImportError:
    from .. import config  # type: ignore

_METRICS = ("voc_index", "temp_c", "humidity_pct")


class History:
    def __init__(self):
        self.bins = config.CHART_BINS
        self.bin_seconds = config.CHART_BIN_SECONDS
        # ring buffer of committed minute-averages (None = no data yet)
        self._series = {m: [None] * self.bins for m in _METRICS}
        self._count = {m: 0 for m in _METRICS}  # filled bins, capped at self.bins
        # accumulator for the current minute
        self._acc_sum = {m: 0.0 for m in _METRICS}
        self._acc_n = {m: 0 for m in _METRICS}
        self._minute_start_s = None

    def add(self, reading, uptime_s):
        """Feed one ~1 s reading. Commits a bin every bin_seconds."""
        if self._minute_start_s is None:
            self._minute_start_s = uptime_s
        for m in _METRICS:
            v = reading.get(m)
            if v is not None:
                self._acc_sum[m] += v
                self._acc_n[m] += 1
        if uptime_s - self._minute_start_s >= self.bin_seconds:
            self._commit()
            self._minute_start_s = uptime_s

    def _commit(self):
        for m in _METRICS:
            avg = (self._acc_sum[m] / self._acc_n[m]) if self._acc_n[m] else None
            self._series[m].pop(0)
            self._series[m].append(avg)
            if avg is not None and self._count[m] < self.bins:
                self._count[m] += 1
            self._acc_sum[m] = 0.0
            self._acc_n[m] = 0

    def series(self, metric):
        """Oldest-to-newest list of minute-averages (length == bins, may hold None)."""
        return self._series[metric]

    def has_data(self, metric):
        return self._count[metric] > 0
