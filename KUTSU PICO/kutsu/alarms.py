"""
Threshold evaluation: turn a raw reading into zero or more alarms.

Pure logic, no hardware — easy to unit-test on a desktop Python too.
"""

try:
    import config
except ImportError:  # allow importing from a non-root context
    from .. import config  # type: ignore


# Severity levels
NORMAL = 0
WARN = 1
HARMFUL = 2

_LEVEL_NAME = {NORMAL: "normal", WARN: "warn", HARMFUL: "harmful"}


def _classify(value, spec):
    """Return (severity, side) for one metric value against its threshold spec."""
    if value is None:
        return NORMAL, None
    hl, wl = spec.get("harmful_low"), spec.get("warn_low")
    wh, hh = spec.get("warn_high"), spec.get("harmful_high")
    if hl is not None and value <= hl:
        return HARMFUL, "low"
    if hh is not None and value >= hh:
        return HARMFUL, "high"
    if wl is not None and value <= wl:
        return WARN, "low"
    if wh is not None and value >= wh:
        return WARN, "high"
    return NORMAL, None


def evaluate(reading, voc_ready=True):
    """
    reading: dict with keys voc_index, temp_c, humidity_pct (any may be None).
    voc_ready: False during SGP40 warm-up -> VOC alarms suppressed.

    Returns dict: {
        "per_metric": {metric: {"severity", "side"}},
        "max_severity": int,
        "harmful": [ {metric, value, message}, ... ],
    }
    """
    per_metric = {}
    harmful = []
    max_sev = NORMAL

    for metric, spec in config.THRESHOLDS.items():
        value = reading.get(metric)
        if metric == "voc_index" and not voc_ready:
            per_metric[metric] = {"severity": NORMAL, "side": None}
            continue
        sev, side = _classify(value, spec)
        per_metric[metric] = {"severity": sev, "side": side}
        if sev > max_sev:
            max_sev = sev
        if sev == HARMFUL:
            harmful.append({
                "metric": metric,
                "value": value,
                "label": spec.get("label", metric),
                "message": spec.get("harmful_msg", "ABNORMAL READING"),
            })

    return {"per_metric": per_metric, "max_severity": max_sev, "harmful": harmful}


def severity_name(level):
    return _LEVEL_NAME.get(level, "?")
