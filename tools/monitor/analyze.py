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
    readings = []
    for t, x in lines:
        if re.fullmatch(r"\d{10}", x):
            board = datetime.fromtimestamp(int(x), timezone.utc).replace(tzinfo=None)
            truth = t.astimezone(LOCAL).replace(tzinfo=None)
            readings.append((t, (board - truth).total_seconds()))
    errors = [e for _, e in readings]
    if not errors:
        report("FAIL", "clock", "no RTC readings in the window")
    else:
        worst = max(errors, key=abs)
        level = "PASS" if abs(worst) <= 10 else ("WARN" if abs(worst) <= 60 else "FAIL")
        report(level, "clock", f"RTC within {abs(worst):.0f} s of real New York time "
                               f"over {len(errors)} readings (latest {errors[-1]:+.0f} s)")
    syncs = [x for t, x in lines if x.startswith("Syncing Clock to: ")]

    # The newest batch in the cloud, for the rows check below and the wedge count it carries.
    rows = []
    path = os.environ.get("LATEST_BATCH")
    if path and os.path.exists(path):
        for line in open(path):
            try:
                rows.append(json.loads(line))
            except ValueError:
                pass

    # The time the board prints as H:M:S in each status block comes from reads of the chip
    # separate from the epoch's, so a flaky bus can garble one and not the other. Its error
    # against New York time, wrapped at midnight.
    hms = []
    for t, x in lines:
        m = re.fullmatch(r"(\d{1,2}):(\d{1,2}):(\d{1,2})", x)
        if m:
            local = t.astimezone(LOCAL)
            board = int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3))
            truth = local.hour * 3600 + local.minute * 60 + local.second
            hms.append((t, (board - truth + 43200) % 86400 - 43200))

    # Wedges, and proof that each one was solved rather than only attempted:
    #   - every wedge detected has a bus clear that brought the chip back,
    #   - the clock read true (epoch and H:M:S within 10 s of New York time) in the first
    #     status block after the heal, before any further wedge,
    #   - no reading was wrong that the detector missed (off by more than a minute, or
    #     "RTC Error", with no detection within a few seconds of it),
    #   - the board's own counters agree (every wedge this power session recovered), and so
    #     does the count the cloud rows carry while the wind sensor is off.
    def count(x, key):
        m = re.search(key + r"=(\d+)", x)
        return int(m.group(1)) if m else None

    detected = [t for t, x in lines if x.startswith("I2C bus unhealthy")]
    clears = [(t, x) for t, x in lines if x.startswith("Bus clear:")]
    healed = [t for t, x in clears if "recovered=1" in x]
    unhealed = [t for t, x in clears if "recovered=0" in x]
    verified, unverified, still_wrong = 0, 0, []
    for h in healed:
        # The first reading after the heal and before the next wedge. Usually the next status
        # block, 10 s on; after a boot it waits on the startup sync, about 25 s.
        until = min(next((d for d in detected if d > h), h + timedelta(seconds=120)), h + timedelta(seconds=120))
        after = [next((e for t, e in series if h <= t <= until), None) for series in (readings, hms)]
        if after[0] is None:
            unverified += 1
        elif all(e is None or abs(e) <= 10 for e in after):
            verified += 1
        else:
            still_wrong.append(f"{h.astimezone(LOCAL):%H:%M:%S} read {max((e for e in after if e is not None), key=abs):+.0f} s "
                               f"after its heal")
    bad = [t for series in (readings, hms) for t, e in series if abs(e) > 60] + \
          [t for t, x in lines if x == "RTC Error"]
    missed = sorted(t for t in bad if not any(-2 <= (d - t).total_seconds() <= 15 for d in detected))
    counted = [(t, count(x, "total_wedges"), count(x, "total_recovered")) for t, x in clears]
    counted = [c for c in counted if c[1] is not None and c[2] is not None]
    totals = {"total_wedges": counted[-1][1], "total_recovered": counted[-1][2]} if counted else {}
    counters_off = bool(totals) and totals["total_wedges"] != totals["total_recovered"]
    detail = (f"{len(detected)} detected, {len(healed)} healed, {len(unhealed)} not healed; "
              f"clock read true after {verified} of {len(healed)} heal(s)")
    if unverified:
        detail += f" ({unverified} with no reading before the next wedge or within 2 min)"
    detail += f"; {len(missed)} wrong reading(s) the detector missed"
    if totals:
        detail += f"; board counts {totals['total_wedges']} wedge(s), {totals['total_recovered']} recovered this power session"
    # Every row in the newest batch, while the wind sensor is off, carries the wedges the board
    # had counted this boot when the row was written. Row times are the RTC's New York time
    # labelled Z; shifting them by the clock's measured error puts them on the capture's
    # timeline. A row within 2 s of a clear could be either side of it, so it may carry the
    # count from just before that clear or just after, and nothing else.
    offset = timedelta(seconds=sorted(errors)[len(errors) // 2]) if errors else timedelta(0)
    cloud_checked, cloud_wrong = 0, []
    placed = []
    for r in rows:
        if r.get("button_panel", {}).get("wind_sensor", True):
            continue
        at = datetime.strptime(r["time"][:19], "%Y-%m-%dT%H:%M:%S").replace(tzinfo=LOCAL) - offset
        if at < lines[0][0]:
            continue
        # A clear whose line was cut short says nothing about the count, so a row it could
        # have changed is left out rather than judged against a stale number.
        cut = [t for t, x in clears if count(x, "total_wedges") is None]
        if any(abs((t - at).total_seconds()) <= 2 for t in cut):
            continue
        near = [n for t, n, _ in counted if abs((t - at).total_seconds()) <= 2]
        if near:
            cloud_checked += 1
            if r["wind_peak"] not in (min(99, near[0] - 1), min(99, near[0])):
                cloud_wrong.append(f"row at {r['time'][11:19]} says {r['wind_peak']}, "
                                   f"the board had {near[0] - 1} or {near[0]}")
            continue
        placed.append((at, r))
        boot = max((b for b in boots if b <= at), default=None)
        last = max((t for t, _ in clears if t <= at and (boot is None or t >= boot)), default=None)
        if last is not None and last in cut:
            continue
        before = [n for t, n, _ in counted if t <= at and (boot is None or t >= boot)]
        if before:
            want = min(99, before[-1])
        elif boot is not None:
            want = 0
        else:
            continue  # the count before this window is not in it
        cloud_checked += 1
        if r["wind_peak"] != want:
            cloud_wrong.append(f"row at {r['time'][11:19]} says {r['wind_peak']}, the board had {want}")
    # Between two rows of the same boot the count rises by exactly the wedges detected in
    # between, so an hour with none must leave it unchanged. This holds with no clear or boot
    # in the window to anchor the count to, which is most hours.
    for (a, ra), (b, rb) in zip(placed, placed[1:]):
        if any(a <= t <= b for t in boots) or rb["wind_peak"] >= 99:
            continue
        between = sum(1 for d in detected if a < d < b)
        cloud_checked += 1
        if rb["wind_peak"] - ra["wind_peak"] != between:
            cloud_wrong.append(f"rows at {ra['time'][11:19]} and {rb['time'][11:19]} say "
                               f"{ra['wind_peak']} then {rb['wind_peak']}, the board detected {between} between")
    if cloud_checked:
        detail += f"; cloud rows agree with the board in {cloud_checked - len(cloud_wrong)} of {cloud_checked}"
    elif rows:
        detail += f"; newest cloud row says {rows[-1]['wind_peak']} (nothing in the window to check it against)"
    cloud_off = bool(cloud_wrong)
    if cloud_wrong:
        detail += " (DISAGREE: " + "; ".join(cloud_wrong) + ")"
    if still_wrong:
        detail += "; " + "; ".join(still_wrong)
    if missed:
        detail += f"; first missed at {missed[0].astimezone(LOCAL):%H:%M:%S}"
    # A capture that restarts can drop a line. A detection without its clear line is still
    # accounted for when a later clear's counters show every wedge this boot recovered; a
    # clear without its detection line is its own proof.
    unaccounted = False
    if len(detected) != len(clears):
        detail += f"; {len(detected)} detections but {len(clears)} bus clear line(s)"
        vouched = counted and not counters_off and counted[-1][0] >= detected[-1]
        unaccounted = len(detected) > len(clears) and not vouched
        if not unaccounted:
            detail += ", a dropped log line; the board's counters account for every wedge"
    failed = unhealed or still_wrong or missed or counters_off or cloud_off
    report("FAIL" if failed else ("WARN" if unverified or unaccounted else "PASS"), "wedges", detail)

    # Alarm: the DS3231 raises its alarm at every hour mark, and the tracker logs re-arming
    # it. A wedge that left the chip half set up shows here as an hour that never rang.
    # The clock's setup logs the same two lines followed by "Setup Clock Module.", so those
    # are a boot or a healed boot wedge, not the alarm. The print can wait on a sync or a
    # download blocking the loop, for as long as the watchdog allows (180 s) and no longer.
    setups = [t for t, x in lines if x == "Setup Clock Module."]
    rings = [t for t, x in lines if x == "Alarm detected and cleared."
             and not any(0 <= (s - t).total_seconds() <= 2 for s in setups)]
    rang, silent = [], []
    mark = (lines[0][0] + timedelta(hours=1)).replace(minute=0, second=0)
    while mark + timedelta(seconds=180) <= lines[-1][0]:
        hit = any(-10 <= (t - mark).total_seconds() <= 180 for t in rings)
        (rang if hit else silent).append(f"{mark.astimezone(LOCAL):%H:%M}")
        mark += timedelta(hours=1)
    if not rang and not silent:
        report("PASS", "alarm", "no hour mark in the window yet")
    else:
        report("FAIL" if silent else "PASS", "alarm",
               f"rang at {', '.join(rang) or 'none'}" + (f"; SILENT at {', '.join(silent)}" if silent else ""))

    # Position: every hour mark in active hours commands exactly the scheduled extension.
    commands = [(t, int(x.split()[-1])) for t, x in lines if re.fullmatch(r"Extending to -?\d+", x)]
    wrong = []
    for t, pct in commands:
        hour = t.astimezone(LOCAL).hour
        if pct == schedule["setSafePercent"]:
            continue
        if not active(hour, schedule):
            # At night the tracker holds still; the only move it makes is to the safe position.
            wrong.append(f"{t.astimezone(LOCAL):%H:%M} commanded {pct}% outside the active hours")
        elif pct != expected_percent(hour, schedule):
            wrong.append(f"{t.astimezone(LOCAL):%H:%M} commanded {pct}%, "
                         f"schedule says {expected_percent(hour, schedule)}%")
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
    # Every status block, not only the last, must agree with the schedule for its hour. SAFE
    # and AWAY are the tracker's own overrides. The first half minute after an hour mark is
    # left out: the status is printed before it is updated, so one block can lag the change.
    off = []
    for t, s in status:
        local = t.astimezone(LOCAL)
        if s in ("ACTIVE", "NIGHT") and local.minute * 60 + local.second > 30 and \
                s != ("ACTIVE" if active(local.hour, schedule) else "NIGHT"):
            off.append((local, s))
    if off:
        detail += f"; status disagreed with the schedule in {len(off)} block(s), " \
                  f"{off[0][0]:%H:%M}-{off[-1][0]:%H:%M} ({off[0][1]})"
    # What the clock actually loaded at boot, which is what the moves follow. A boot that ran
    # on the defaults instead of the card's schedule shows here.
    s = {k: int(v) for k, v in schedule.items()}
    want_loaded = (f"Schedule: {s['setStartHour']}h at {s['setStartPercent']}%, "
                   f"{s['setMiddleHour']}h at {s['setMiddlePercent']}%, "
                   f"{s['setEndHour']}h at {s['setEndPercent']}%, safe {s['setSafePercent']}%")
    loaded_wrong = [x for t, x in lines if x.startswith("Schedule: ") and x != want_loaded]
    if loaded_wrong:
        detail += f"; booted with '{loaded_wrong[-1]}', the card says '{want_loaded}'"
    report("FAIL" if wrong or missed or off or loaded_wrong else "PASS", "position",
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
    # One malformed line must not silence the hourly check: a crash is reported as a FAIL.
    try:
        sys.exit(main())
    except Exception as e:
        print(f"FAIL  analyze   crashed on this window: {type(e).__name__}: {e}")
        sys.exit(1)
