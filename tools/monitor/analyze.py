"""Judge one window of the tracker's serial log, and its uploads, against what it should do.

    python3 analyze.py <since ISO> <log file> <object list file> <schedule.json> [until ISO]

The log is the home PC's capture: every line "<UTC ISO> <text>". The object list is the
names of the telemetry batches in Cloud Storage. Prints one line per check, PASS, WARN or
FAIL, and exits 1 if any FAIL.
"""

import json
import os
import re
import sys
from datetime import datetime, timedelta, timezone
from zoneinfo import ZoneInfo

LOCAL = ZoneInfo("America/New_York")  # the clock is set from the server's /time/est


def parse_time(text):
    return datetime.strptime(text, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc)


def percent_of_day(hour, s):
    # Clock.cpp returns an int, so this truncates like the firmware does.
    return int((hour - s["setStartHour"]) / (s["setEndHour"] - s["setStartHour"]) * 100)


def expected_percent(hour, s):
    """ClockModule::getHourlyExtensionPercent(), step for step."""
    day = percent_of_day(hour, s)
    start_pct, finish_pct = s["setMiddlePercent"], s["setEndPercent"]
    start_hour, finish_hour = s["setMiddleHour"], s["setEndHour"]
    if day < percent_of_day(s["setMiddleHour"], s):
        start_pct, finish_pct = s["setStartPercent"], s["setMiddlePercent"]
        start_hour, finish_hour = s["setStartHour"], s["setMiddleHour"]
    lo, hi = percent_of_day(start_hour, s), percent_of_day(finish_hour, s)
    interval = (day - lo) * 100 / (hi - lo)
    return int((finish_pct - start_pct) * (interval / 100) + start_pct)


def active(hour, s):
    return s["setStartHour"] <= hour <= s["setEndHour"]


def main():
    since = parse_time(sys.argv[1])
    # A past window can be judged too, as if it were checked at `until`.
    now = parse_time(sys.argv[5]) if len(sys.argv) > 5 else datetime.now(timezone.utc)
    lines = []
    for raw in open(sys.argv[2], encoding="utf-8", errors="replace"):
        m = re.match(r"^(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ) (.*)$", raw.rstrip("\n"))
        if m and since <= parse_time(m.group(1)) <= now:
            lines.append((parse_time(m.group(1)), m.group(2)))
    objects = [l.strip() for l in open(sys.argv[3]) if l.strip()]
    schedule = json.load(open(sys.argv[4]))
    results = []

    def report(level, name, detail):
        results.append(level)
        print(f"{level:4}  {name:9} {detail}")

    if not lines:
        report("FAIL", "alive", f"no serial lines since {sys.argv[1]}")
        return 1
    span_min = (lines[-1][0] - lines[0][0]).total_seconds() / 60

    # Alive: the ten second status block keeps coming.
    status = [(t, x.split(": ", 1)[1]) for t, x in lines if x.startswith("Status: ")]
    if status and (now - status[-1][0]).total_seconds() <= 120:
        report("PASS", "alive", f"status every 10 s, last {int((now - status[-1][0]).total_seconds())} s ago, "
                                f"{len(status)} blocks over {span_min:.0f} min")
    else:
        report("FAIL", "alive", "no status block in the last 2 minutes")

    # Restarts: the tracker should not restart on its own.
    boots = [t for t, x in lines if x.startswith("Firmware version: ")]
    causes = [x.split("last reset: ")[1] for t, x in lines if "Watchdog armed" in x and "last reset: " in x]
    version = next((x.split(": ", 1)[1] for t, x in reversed(lines) if x.startswith("Firmware version: ")), None)
    if not boots:
        report("PASS", "restarts", "none")
    else:
        report("WARN" if len(boots) == 1 else "FAIL", "restarts",
               f"{len(boots)} ({', '.join(causes) or 'cause not logged'}), running {version}")

    # Clock: the board prints its RTC as a local-time epoch every 10 s. Compared with the
    # capture's own UTC stamp, shifted to New York time.
    errors = []
    for t, x in lines:
        if re.fullmatch(r"\d{10}", x):
            board = datetime.fromtimestamp(int(x), timezone.utc).replace(tzinfo=None)
            truth = t.astimezone(LOCAL).replace(tzinfo=None)
            errors.append((board - truth).total_seconds())
    if not errors:
        report("FAIL", "clock", "no RTC readings in the window")
    else:
        worst = max(errors, key=abs)
        level = "PASS" if abs(worst) <= 10 else ("WARN" if abs(worst) <= 60 else "FAIL")
        report(level, "clock", f"RTC within {abs(worst):.0f} s of real New York time "
                               f"over {len(errors)} readings (latest {errors[-1]:+.0f} s)")
    syncs = [x for t, x in lines if x.startswith("Syncing Clock to: ")]

    # Wedges: an unhealthy I2C bus, and whether the bus clear brought it back.
    wedges = sum(1 for t, x in lines if x.startswith("I2C bus unhealthy"))
    fixed = sum(1 for t, x in lines if x.startswith("Bus clear:") and "recovered=1" in x)
    unfixed = sum(1 for t, x in lines if x.startswith("Bus clear:") and "recovered=0" in x)
    totals = next((x for t, x in reversed(lines) if x.startswith("Bus clear:")), "")
    detail = f"{wedges} wedge(s), {fixed} healed, {unfixed} not healed"
    if totals:
        detail += " (" + " ".join(p for p in totals.split() if p.startswith("total_")) + ")"
    report("PASS" if unfixed == 0 else "FAIL", "wedges", detail)

    # Position: every hour mark in active hours commands exactly the scheduled extension.
    commands = [(t, int(x.split()[-1])) for t, x in lines if re.fullmatch(r"Extending to -?\d+", x)]
    wrong = []
    for t, pct in commands:
        hour = t.astimezone(LOCAL).hour
        want = expected_percent(hour, schedule) if active(hour, schedule) else None
        if want is not None and pct != want and pct != schedule["setSafePercent"]:
            wrong.append(f"{t.astimezone(LOCAL):%H:%M} commanded {pct}%, schedule says {want}%")
    missed = []
    mark = (lines[0][0] + timedelta(hours=1)).replace(minute=0, second=0)
    while mark + timedelta(minutes=3) <= lines[-1][0]:
        hour = mark.astimezone(LOCAL).hour
        if active(hour, schedule) and not any(0 <= (t - mark).total_seconds() <= 180 for t, _ in commands):
            missed.append(f"{mark.astimezone(LOCAL):%H:%M}")
        mark += timedelta(hours=1)
    states = sorted({s for _, s in status})
    local_now = lines[-1][0].astimezone(LOCAL)
    want_state = "ACTIVE" if active(local_now.hour, schedule) else "NIGHT"
    detail = (", ".join(f"{t.astimezone(LOCAL):%H:%M} {p}%" for t, p in commands) or "no move commanded") + \
             f"; status {'/'.join(states)} (schedule says {want_state} at {local_now:%H:%M})"
    extended = [x for t, x in lines if x.startswith("PercentExtended: ")]
    if extended:
        detail += f"; actuator reports {extended[-1].split(': ')[1]}%"
    bad_state = status and status[-1][1] not in (want_state, "SAFE", "AWAY")
    report("FAIL" if wrong or missed or bad_state else "PASS", "position",
           detail + ("; " + "; ".join(wrong) if wrong else "") + ("; no command at " + ", ".join(missed) if missed else ""))

    # Uploads: a batch every fifteen minutes, each one landing in Cloud Storage.
    uploaded = [int(re.search(r"Uploaded (\d+)", x).group(1)) for t, x in lines if re.match(r"Uploaded \d+ chunk", x)]
    failed = sum(1 for t, x in lines if "failed, keeping it" in x)
    batches = []
    for name in objects:
        m = re.search(r"(\d{4}-\d\d-\d\dT\d\d)-(\d\d)-(\d\d)-\d+Z\.ndjson$", name)
        if m:
            at = parse_time(f"{m.group(1)}:{m.group(2)}:{m.group(3)}Z")
            if lines[0][0] <= at <= now:
                batches.append(at)
    expected = max(1, int(span_min // 15))
    level = "PASS" if len(batches) >= expected - 1 and failed == 0 else ("WARN" if batches else "FAIL")
    report(level, "uploads", f"{len(uploaded)} sync(s) posted {sum(uploaded)} chunk(s), {failed} failed; "
                             f"{len(batches)} batch(es) in Cloud Storage over {span_min:.0f} min (expect ~{expected}); "
                             f"{len(syncs)} clock sync(s)")

    # Rows: the newest batch's rows carry the board's local time (labelled Z by the server),
    # a status, and the extension. Each row should be recent and in order.
    rows = []
    path = os.environ.get("LATEST_BATCH")
    if path and os.path.exists(path):
        for line in open(path):
            try:
                rows.append(json.loads(line))
            except ValueError:
                pass
    if not rows:
        report("WARN", "rows", "could not read the newest batch")
    else:
        stamps = [datetime.strptime(r["time"][:19], "%Y-%m-%dT%H:%M:%S") for r in rows]
        local_now = now.astimezone(LOCAL).replace(tzinfo=None)
        age_min = (local_now - stamps[-1]).total_seconds() / 60
        ordered = all(a < b for a, b in zip(stamps, stamps[1:]))
        last = rows[-1]
        level = "PASS" if 0 <= age_min <= 25 and ordered else "FAIL"
        report(level, "rows", f"newest batch has {len(rows)} row(s), last at {stamps[-1]:%H:%M} local "
                              f"({age_min:.0f} min old{'' if ordered else ', OUT OF ORDER'}): "
                              f"status {last['status']}, extended {last['extended_percent']}%, "
                              f"wind/wedge column {last['wind_peak']}")

    # Memory: the free gap between heap and stack. A steady fall is a leak or fragmentation.
    free = [(t, int(x.split(": ")[1])) for t, x in lines if re.fullmatch(r"Free RAM: -?\d+", x)]
    if not free:
        report("WARN", "memory", "no Free RAM lines (firmware older than the one that prints them)")
    else:
        low = min(v for _, v in free)
        drift = free[-1][1] - free[0][1]
        level = "FAIL" if low < 2048 else ("WARN" if drift < -512 else "PASS")
        report(level, "memory", f"{free[-1][1]} bytes free, lowest {low}, {drift:+d} over the window")

    # OTA: what the updater last said, and anything alarming.
    ota = [x for t, x in lines if x.startswith("OTA: ")]
    alarms = [x for x in ota if re.search(r"ROLLING BACK|CANNOT|failed \(", x)]
    last = ota[-1] if ota else "no OTA lines"
    report("WARN" if alarms else "PASS", "ota", f"running {version or 'unchanged'}; last: {last[:110]}" +
           (f"; {len(alarms)} alarm line(s): {alarms[-1][:90]}" if alarms else ""))

    return 1 if "FAIL" in results else 0


if __name__ == "__main__":
    sys.exit(main())
